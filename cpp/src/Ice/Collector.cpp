// **********************************************************************
//
// Copyright (c) 2001
// MutableRealms, Inc.
// Huntsville, AL, USA
//
// All Rights Reserved
//
// **********************************************************************

#include <Ice/Collector.h>
#include <Ice/Instance.h>
#include <Ice/Logger.h>
#include <Ice/Properties.h>
#include <Ice/TraceUtil.h>
#include <Ice/Transceiver.h>
#include <Ice/Acceptor.h>
#include <Ice/ThreadPool.h>
#include <Ice/ObjectAdapter.h>
#include <Ice/Endpoint.h>
#include <Ice/Incoming.h>
#include <Ice/Exception.h>
#include <Ice/Protocol.h>
#include <Ice/Functional.h>
#include <Ice/SecurityException.h> // TODO: bandaid, see below.

using namespace std;
using namespace Ice;
using namespace IceInternal;

void IceInternal::incRef(Collector* p) { p->__incRef(); }
void IceInternal::decRef(Collector* p) { p->__decRef(); }

void IceInternal::incRef(CollectorFactory* p) { p->__incRef(); }
void IceInternal::decRef(CollectorFactory* p) { p->__decRef(); }

void
IceInternal::Collector::destroy()
{
    JTCSyncT<JTCRecursiveMutex> sync(*this);
    setState(StateClosing);
}

bool
IceInternal::Collector::destroyed() const
{
    JTCSyncT<JTCRecursiveMutex> sync(*this);
    return _state >= StateClosing;
}

void
IceInternal::Collector::hold()
{
    JTCSyncT<JTCRecursiveMutex> sync(*this);
    setState(StateHolding);
}

void
IceInternal::Collector::activate()
{
    JTCSyncT<JTCRecursiveMutex> sync(*this);
    setState(StateActive);
}

bool
IceInternal::Collector::server() const
{
    return true;
}

bool
IceInternal::Collector::readable() const
{
    return true;
}

void
IceInternal::Collector::read(BasicStream& stream)
{
    _transceiver->read(stream, 0);
}

void
IceInternal::Collector::message(BasicStream& stream)
{
    Incoming in(_instance, _adapter);
    BasicStream* is = in.is();
    BasicStream* os = in.os();
    stream.swap(*is);
    bool invoke = false;
    bool batch = false;
    bool response = false;

    {
	JTCSyncT<JTCRecursiveMutex> sync(*this);

	_threadPool->promoteFollower();

	if (_state != StateActive && _state != StateClosing)
	{
	    return;
	}
	
	try
	{
	    assert(is->i == is->b.end());
	    is->i = is->b.begin() + 2;
	    Byte messageType;
	    is->read(messageType);
	    is->i = is->b.begin() + headerSize;

	    //
	    // Write partial message header
	    //
	    os->write(protocolVersion);
	    os->write(encodingVersion);

	    switch (messageType)
	    {
		case requestMsg:
		{
		    if (_state == StateClosing)
		    {
			traceRequest("received request during closing\n"
				     "(ignored by server, client will retry)",
				     *is, _logger, _traceLevels);
		    }
		    else
		    {
			traceRequest("received request",
				     *is, _logger, _traceLevels);
			invoke = true;
			Int requestId;
			is->read(requestId);
			if (!_endpoint->oneway() && requestId != 0) // 0 means oneway
			{
			    response = true;
			    ++_responseCount;
			    os->write(replyMsg);
			    os->write(Int(0)); // Message size (placeholder)
			    os->write(requestId);
			}
		    }
		    break;
		}
		
		case requestBatchMsg:
		{
		    if (_state == StateClosing)
		    {
			traceBatchRequest("received batch request during closing\n"
					  "(ignored by server, client will retry)",
					  *is, _logger, _traceLevels);
		    }
		    else
		    {
			traceBatchRequest("received batch request",
					  *is, _logger, _traceLevels);
			invoke = true;
			batch = true;
		    }
		    break;
		}
		
		case replyMsg:
		{
		    traceReply("received reply on server side\n"
			       "(invalid, closing connection)",
			       *is, _logger, _traceLevels);
		    throw InvalidMessageException(__FILE__, __LINE__);
		    break;
		}
		
		case closeConnectionMsg:
		{
		    traceHeader("received close connection on server side\n"
				"(invalid, closing connection)",
				*is, _logger, _traceLevels);
		    throw InvalidMessageException(__FILE__, __LINE__);
		    break;
		}
		
		default:
		{
		    traceHeader("received unknown message\n"
				"(invalid, closing connection)",
				*is, _logger, _traceLevels);
		    throw UnknownMessageException(__FILE__, __LINE__);
		    break;
		}
	    }
	}
	catch (const ConnectionLostException&)
	{
	    setState(StateClosed); // Connection drop from client is ok
	    return;
	}
	catch (const LocalException& ex)
	{
	    warning(ex);
	    setState(StateClosed);
	    return;
	}
    }

    if (invoke)
    {
	do
	{
	    try
	    {
		in.invoke();
	    }
	    catch (const Exception& ex)
	    {
		JTCSyncT<JTCRecursiveMutex> sync(*this);
		warning(ex);
	    }
	    catch (...)
	    {
		assert(false); // Should not happen
	    }
	}
	while (batch && is->i < is->b.end());
    }

    if (response)
    {
	JTCSyncT<JTCRecursiveMutex> sync(*this);
	
	if (_state != StateActive && _state != StateClosing)
	{
	    return;
	}
	
	try
	{
	    os->i = os->b.begin();
	    
	    //
	    // Fill in the message size
	    //
	    const Byte* p;
	    Int sz = os->b.size();
	    p = reinterpret_cast<Byte*>(&sz);
	    copy(p, p + sizeof(Int), os->i + 3);
	    
	    traceReply("sending reply", *os, _logger, _traceLevels);
	    _transceiver->write(*os, _endpoint->timeout());
	    
	    --_responseCount;

	    if (_state == StateClosing && _responseCount == 0)
	    {
		closeConnection();
	    }
	}
	catch (const ConnectionLostException&)
	{
	    setState(StateClosed); // Connection drop from client is ok
	    return;
	}
	catch (const LocalException& ex)
	{
	    warning(ex);
	    setState(StateClosed);
	    return;
	}
    }
}

void
IceInternal::Collector::exception(const LocalException& ex)
{
    JTCSyncT<JTCRecursiveMutex> sync(*this);

    if (_state != StateActive && _state != StateClosing)
    {
	return;
    }

    if (!dynamic_cast<const ConnectionLostException*>(&ex))
    {
	warning(ex);
    }

    setState(StateClosed);
}

void
IceInternal::Collector::finished()
{
    JTCSyncT<JTCRecursiveMutex> sync(*this);

    //
    // We also unregister with the thread poool if we go to holding
    // state, but in this case we may not close the connection.
    //
    if (_state == StateClosed)
    {
	_transceiver->close();
    }
}

bool
IceInternal::Collector::tryDestroy()
{
    bool isLocked = trylock();
    if(!isLocked)
    {
	return false;
    }

    _threadPool->promoteFollower();

    try
    {
	setState(StateClosing);
    }
    catch (...)
    {
	unlock();
	throw;
    }
    
    unlock();    
    return true;
}

IceInternal::Collector::Collector(const InstancePtr& instance,
				  const ObjectAdapterPtr& adapter,
				  const TransceiverPtr& transceiver,
				  const EndpointPtr& endpoint) :
    EventHandler(instance),
    _adapter(adapter),
    _transceiver(transceiver),
    _endpoint(endpoint),
    _traceLevels(instance->traceLevels()),
    _logger(instance->logger()),
    _responseCount(0),
    _state(StateHolding)
{
    _warnAboutExceptions =
	atoi(_instance->properties()->getProperty("Ice.WarnAboutServerExceptions").c_str()) > 0 ? true : false;

    _threadPool = _instance->threadPool();
}

IceInternal::Collector::~Collector()
{
    assert(_state == StateClosed);
}

void
IceInternal::Collector::setState(State state)
{
    if (_endpoint->oneway() && state == StateClosing)
    {
	state = StateClosed;
    }
    
    if (_state == state) // Don't switch twice
    {
	return;
    }
    
    switch (state)
    {
	case StateActive:
	{
	    if (_state != StateHolding) // Can only switch from holding to active
	    {
		return;
	    }

	    _threadPool->_register(_transceiver->fd(), this);
	    break;
	}
	
	case StateHolding:
	{
	    if (_state != StateActive) // Can only switch from active to holding
	    {
		return;
	    }

	    _threadPool->unregister(_transceiver->fd());
	    break;
	}

	case StateClosing:
	{
	    if (_state == StateClosed) // Can't change back from closed
	    {
		return;
	    }

	    if (_responseCount == 0)
	    {
		try
		{
		    closeConnection();
		}
		catch (const ConnectionLostException&)
		{
		    state = StateClosed;
		    setState(state); // Connection drop from client is ok
		}
		catch (const LocalException& ex)
		{
		    warning(ex);
		    state = StateClosed;
		    setState(state);
		}
	    }
	    
	    //
	    // We need to continue to read data in closing state
	    //
	    if (_state == StateHolding)
	    {
		_threadPool->_register(_transceiver->fd(), this);
	    }
	    break;
	}

	case StateClosed:
	{
	    //
	    // If we come from holding state, we are already unregistered,
	    // so we can close right away.
	    //
	    if (_state == StateHolding)
	    {
		_transceiver->close();
	    }
	    else
	    {
		_threadPool->unregister(_transceiver->fd());
	    }
	    break;
	}
    }

    _state = state;
}

void
IceInternal::Collector::closeConnection()
{
    BasicStream os(_instance);
    os.write(protocolVersion);
    os.write(encodingVersion);
    os.write(closeConnectionMsg);
    os.write(headerSize); // Message size
    os.i = os.b.begin();
    traceHeader("sending close connection", os, _logger, _traceLevels);
    _transceiver->write(os, _endpoint->timeout());
    _transceiver->shutdown();
}

void
IceInternal::Collector::warning(const Exception& ex) const
{
    if (_warnAboutExceptions)
    {
	ostringstream s;
	s << "server exception:\n" << ex << '\n' << _transceiver->toString();
	_logger->warning(s.str());
    }
}

void
IceInternal::CollectorFactory::destroy()
{
    JTCSyncT<JTCMutex> sync(*this);
    setState(StateClosed);
}

void
IceInternal::CollectorFactory::hold()
{
    JTCSyncT<JTCMutex> sync(*this);
    setState(StateHolding);
}

void
IceInternal::CollectorFactory::activate()
{
    JTCSyncT<JTCMutex> sync(*this);
    setState(StateActive);
}

EndpointPtr
IceInternal::CollectorFactory::endpoint() const
{
    return _endpoint;
}

bool
IceInternal::CollectorFactory::equivalent(const EndpointPtr& endp) const
{
    if (_transceiver)
    {
	return endp->equivalent(_transceiver);
    }
    
    assert(_acceptor);
    return endp->equivalent(_acceptor);
}

bool
IceInternal::CollectorFactory::server() const
{
    return true;
}

bool
IceInternal::CollectorFactory::readable() const
{
    return false;
}

void
IceInternal::CollectorFactory::read(BasicStream&)
{
    assert(false); // Must not be called
}

void
IceInternal::CollectorFactory::message(BasicStream&)
{
    JTCSyncT<JTCMutex> sync(*this);

    _threadPool->promoteFollower();

    if (_state != StateActive)
    {
	return;
    }
    
    //
    // First reap destroyed collectors
    //
    // Can't use _collectors.remove_if(constMemFun(...)), because VC++
    // doesn't support member templates :-(
    _collectors.erase(remove_if(_collectors.begin(), _collectors.end(), ::Ice::constMemFun(&Collector::destroyed)),
		      _collectors.end());

    //
    // Now accept a new connection and create a new CollectorPtr
    //
    try
    {
	TransceiverPtr transceiver = _acceptor->accept(0);
	CollectorPtr collector = new Collector(_instance, _adapter, transceiver, _endpoint);
	collector->activate();
	_collectors.push_back(collector);
    }
    catch (const IceSecurity::SecurityException& ex)
    {
        // TODO: bandaid. Takes care of SSL Handshake problems during
        // creation of a Transceiver. Ignore, nothing we can do here.
        warning(ex);
    }
    catch (const SocketException& ex)
    {
        // TODO: bandaid. Takes care of SSL Handshake problems during
        // creation of a Transceiver. Ignore, nothing we can do here.
        warning(ex);
    }
    catch (const TimeoutException&)
    {
	// Ignore timeouts
    }
    catch (const LocalException& ex)
    {
	warning(ex);
	destroy();
    }
}

void
IceInternal::CollectorFactory::exception(const LocalException&)
{
    assert(false); // Must not be called
}

void
IceInternal::CollectorFactory::finished()
{
    JTCSyncT<JTCMutex> sync(*this);

    //
    // We also unregister with the thread poool if we go to holding
    // state, but in this case we may not close the acceptor.
    //
    if (_state == StateClosed)
    {
	_acceptor->shutdown();
	clearBacklog();
	_acceptor->close();
    }
}

bool
IceInternal::CollectorFactory::tryDestroy()
{
    //
    // Do nothing. We don't want collector factories to be closed by
    // active connection management.
    //
    return false;
}

IceInternal::CollectorFactory::CollectorFactory(const InstancePtr& instance,
						const ObjectAdapterPtr& adapter,
						const EndpointPtr& endpoint) :
    EventHandler(instance),
    _adapter(adapter),
    _endpoint(endpoint),
    _traceLevels(instance->traceLevels()),
    _logger(instance->logger()),
    _state(StateHolding)
{
    _warnAboutExceptions =
	atoi(_instance->properties()->getProperty("Ice.WarnAboutServerExceptions").c_str()) > 0 ? true : false;

    try
    {
	_transceiver = _endpoint->serverTransceiver(_instance, _endpoint);
	if (_transceiver)
	{
	    CollectorPtr collector = new Collector(_instance, _adapter, _transceiver, _endpoint);
	    _collectors.push_back(collector);
	}
	else
	{
	    _acceptor = _endpoint->acceptor(_instance, _endpoint);
	    assert(_acceptor);
	    _acceptor->listen();
	    _threadPool = _instance->threadPool();
	}
    }
    catch (...)
    {
	setState(StateClosed);
	throw;
    }
}

IceInternal::CollectorFactory::~CollectorFactory()
{
    assert(_state == StateClosed);
}

void
IceInternal::CollectorFactory::setState(State state)
{
    if (_state == state) // Don't switch twice
    {
	return;
    }

    switch (state)
    {
	case StateActive:
	{
	    if (_state != StateHolding) // Can only switch from holding to active
	    {
		return;
	    }

	    if (_threadPool)
	    {
		_threadPool->_register(_acceptor->fd(), this);
	    }

	    for_each(_collectors.begin(), _collectors.end(), ::Ice::voidMemFun(&Collector::activate));
	    break;
	}
	
	case StateHolding:
	{
	    if (_state != StateActive) // Can only switch from active to holding
	    {
		return;
	    }

	    if (_threadPool)
	    {
		_threadPool->unregister(_acceptor->fd());
	    }

	    for_each(_collectors.begin(), _collectors.end(), ::Ice::voidMemFun(&Collector::hold));
	    break;
	}
	
	case StateClosed:
	{
	    if (_threadPool)
	    {
		//
		// If we come from holding state, we are already
		// unregistered, so we can close right away.
		//
		if (_state == StateHolding)
		{
		    _acceptor->shutdown();
		    clearBacklog();
		    _acceptor->close();
		}
		else
		{
		    _threadPool->unregister(_acceptor->fd());
		}
	    }
	    for_each(_collectors.begin(), _collectors.end(), ::Ice::voidMemFun(&Collector::destroy));
	    _collectors.clear();
	    break;
	}
    }

    _state = state;
}

void
IceInternal::CollectorFactory::clearBacklog()
{
    //
    // Clear listen() backlog properly by accepting all queued
    // connections, and then shutting them down.
    //
    while (true)
    {
	try
	{
	    TransceiverPtr transceiver = _acceptor->accept(0);
	    CollectorPtr collector = new Collector(_instance, _adapter, transceiver, _endpoint);
	    collector->destroy();
	}
	catch (const Exception&)
	{
	    break;
	}
    }
}

void
IceInternal::CollectorFactory::warning(const Exception& ex) const
{
    if (_warnAboutExceptions)
    {
	ostringstream s;
	s << "server exception:\n" << ex << '\n' << _acceptor->toString();
	_logger->warning(s.str());
    }
}
