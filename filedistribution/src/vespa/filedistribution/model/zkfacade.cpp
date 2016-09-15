// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#include <vespa/fastos/fastos.h>

#include "zkfacade.h"

#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <cassert>
#include <cstdio>
#include <sstream>
#include <thread>
#include <boost/throw_exception.hpp>
#include <boost/function_output_iterator.hpp>

#include <zookeeper/zookeeper.h>
#include <vespa/filedistribution/common/logfwd.h>
#include <vespa/defaults.h>
#include <vespa/vespalib/util/sync.h>

typedef std::unique_lock<std::mutex> UniqueLock;

using filedistribution::ZKFacade;
using filedistribution::Move;
using filedistribution::Buffer;
using filedistribution::ZKGenericException;
using filedistribution::ZKLogging;

typedef ZKFacade::Path Path;

namespace {

class RetryController {
    unsigned int _retryCount;
    ZKFacade& _zkFacade;

    static const unsigned int _maxRetries = 10;
public:
    int _lastStatus;

    RetryController(ZKFacade* zkFacade)
    :_retryCount(0),
     _zkFacade(*zkFacade),
     _lastStatus(0)
    {}

    void operator()(int status) {
        _lastStatus = status;
    }

    bool shouldRetry() {
        ++_retryCount;

        return  _zkFacade.retriesEnabled() &&
                _lastStatus != ZOK &&
                _retryCount < _maxRetries &&
                isNonLastingError(_lastStatus) &&
                pause();
    }

    bool isNonLastingError(int error) {
        return error == ZCONNECTIONLOSS ||
            error == ZOPERATIONTIMEOUT;
    }

    bool pause() {
        unsigned int sleepInSeconds = 1;
        sleep(sleepInSeconds);
        LOGFWD(info, "Retrying zookeeper operation.");
        return true;
    }

    void throwIfError() {
        namespace fd = filedistribution;

        switch (_lastStatus) {
          case ZSESSIONEXPIRED:
            BOOST_THROW_EXCEPTION(fd::ZKSessionExpired());
          case ZNONODE:
            BOOST_THROW_EXCEPTION(fd::ZKNodeDoesNotExistsException());
          case ZNODEEXISTS:
            BOOST_THROW_EXCEPTION(fd::ZKNodeExistsException());
          default:
            if (_lastStatus != ZOK)
                BOOST_THROW_EXCEPTION(fd::ZKGenericException(_lastStatus));
        }
    }
};

class DeallocateZKStringVectorGuard {
    String_vector& _strings;
public:
    DeallocateZKStringVectorGuard(String_vector& strings)
        :_strings(strings)
    {}

    ~DeallocateZKStringVectorGuard() {
        deallocate_String_vector(&_strings);
    }
};

const Path
setDataForNewFile(ZKFacade& zk, const Path& path, const char* buffer, int length, zhandle_t* zhandle, int createFlags)  {

    RetryController retryController(&zk);
    const int maxPath = 1024;
    char createdPath[maxPath];
    do {
        retryController(
                zoo_create(zhandle, path.string().c_str(),
                buffer, length,
                &ZOO_OPEN_ACL_UNSAFE, createFlags,
                createdPath, maxPath));
    } while (retryController.shouldRetry());

    retryController.throwIfError();
    return Path(createdPath);
}

void
setDataForExistingFile(ZKFacade& zk, const Path& path, const char* buffer, int length, zhandle_t* zhandle)  {
    RetryController retryController(&zk);

    const int ignoreVersion = -1;
    do {
        retryController(
                zoo_set(zhandle, path.string().c_str(),
                        buffer, length, ignoreVersion));
    } while (retryController.shouldRetry());

    retryController.throwIfError();
}

} //anonymous namespace

/********** Active watchers *******************************************/
struct ZKFacade::ZKWatcher {
    const std::weak_ptr<ZKFacade> _owner;
    const NodeChangedWatcherSP _nodeChangedWatcher;

    ZKWatcher(
        const std::shared_ptr<ZKFacade> &owner,
        const NodeChangedWatcherSP& nodeChangedWatcher )
    :_owner(owner),
    _nodeChangedWatcher(nodeChangedWatcher)
    {}

    static void watcherFn(zhandle_t *zh, int type,
        int state, const char *path,void *watcherContext) {

        (void)zh;
        (void)state;
        (void)path;

        if (type == ZOO_SESSION_EVENT) {
            //The session events do not cause unregistering of the watcher
            //inside zookeeper, so don't unregister it here.
            LOGFWD(debug, "ZKWatcher recieved session event with state '%d'. Ignoring", state);
            return;
        }

        LOGFWD(debug, "ZKWatcher: Begin watcher called for path '%s' with type %d.", path, type);

        ZKWatcher* self = static_cast<ZKWatcher*>(watcherContext);

        //WARNING: Since we're creating a shared_ptr to ZKFacade here, this might cause
        //destruction of the ZKFacade in a zookeeper thread.
        //Since zookeeper_close blocks until all watcher threads are finished, and we're inside a watcher thread,
        //this will cause infinite waiting.
        //To avoid this, a custom shared_ptr deleter using a separate deleter thread must be used.

        if (std::shared_ptr<ZKFacade> zk = self->_owner.lock()) {
            zk->invokeWatcher(watcherContext);
        }

        LOGFWD(debug, "ZKWatcher: End watcher called for path '%s' with type %d.", path, type);
    }
};

void
ZKFacade::stateWatchingFun(zhandle_t*, int type, int state, const char* path, void* context) {
    (void)path;
    (void)context;

    //The ZKFacade won't expire before zookeeper_close has finished.
    if (type == ZOO_SESSION_EVENT) {
        LOGFWD(debug, "Zookeeper session event: %d", state);
        if (state == ZOO_EXPIRED_SESSION_STATE) {
            throw ZKSessionExpired();
        } else if (state == ZOO_AUTH_FAILED_STATE) {
            throw ZKGenericException(ZNOAUTH);
        }
    } else {
        LOGFWD(info, "State watching function: Unexpected event: '%d' -- '%d' ",  type, state);
    }
}


void* /* watcherContext */
ZKFacade::registerWatcher(const NodeChangedWatcherSP& watcher) {

    UniqueLock lock(_watchersMutex);
    std::shared_ptr<ZKWatcher> zkWatcher(new ZKWatcher(shared_from_this(), watcher));
    _watchers[zkWatcher.get()] = zkWatcher;
    return zkWatcher.get();
}


std::shared_ptr<ZKFacade::ZKWatcher>
ZKFacade::unregisterWatcher(void* watcherContext) {
    UniqueLock lock(_watchersMutex);

    WatchersMap::iterator i = _watchers.find(watcherContext);
    if (i == _watchers.end()) {
        return std::shared_ptr<ZKWatcher>();
    } else {
        std::shared_ptr<ZKWatcher> result = i->second;
        _watchers.erase(i);
        return result;
    }
}

void
ZKFacade::invokeWatcher(void* watcherContext) {
    std::shared_ptr<ZKWatcher> watcher = unregisterWatcher(watcherContext);

    if (!_watchersEnabled)
        return;

    if (watcher) {
        (*watcher->_nodeChangedWatcher)();
    } else {
        LOGFWD(error, "Invoke called on expired watcher.");
    }
}

/********** End live watchers ***************************************/


ZKFacade::ZKFacade(const std::string& zkservers)
    :_retriesEnabled(true),
     _watchersEnabled(true),
     _zhandle(zookeeper_init(zkservers.c_str(),
                             &ZKFacade::stateWatchingFun,
                             _zkSessionTimeOut,
                             0, //clientid,
                             this, //context,
                             0)) //flags
{
    if (!_zhandle) {
        BOOST_THROW_EXCEPTION(ZKFailedConnecting());
    }
}

ZKFacade::~ZKFacade() {
    disableRetries();
    _watchersEnabled = false;
    vespalib::Gate done;
    std::thread closer([&done, zhandle=_zhandle] () { zookeeper_close(zhandle); done.countDown(); });
    if ( done.await(50*1000) ) {
        LOGFWD(debug, "Zookeeper connection closed successfully.");
    } else {
        LOGFWD(error, "Not able to close down zookeeper. Dumping core so you can figure out what is wrong");
        abort();
    }
    closer.join();
}

const std::string
ZKFacade::getString(const Path& path) {
    Buffer buffer(getData(path));
    return std::string(buffer.begin(), buffer.end());
}

const Move<Buffer>
ZKFacade::getData(const Path& path) {
    RetryController retryController(this);
    try {
        Buffer buffer(_maxDataSize);
        int bufferSize = _maxDataSize;

        const int watchIsOff = 0;
        do {
            Stat stat;
            bufferSize = _maxDataSize;

            retryController(
                    zoo_get(_zhandle, path.string().c_str(), watchIsOff,
                            &*buffer.begin(),
                            &bufferSize, //in & out
                            &stat));
        } while(retryController.shouldRetry());

        retryController.throwIfError();

        buffer.resize(bufferSize);
        return move(buffer);

    } catch(boost::exception& e) {
        e <<errorinfo::Path(path);
        throw;
    }
}

const Move<Buffer>
ZKFacade::getData(const Path& path, const NodeChangedWatcherSP& watcher) {
    void* watcherContext = registerWatcher(watcher);
    RetryController retryController(this);

    try {
        Buffer buffer(_maxDataSize);
        int bufferSize = _maxDataSize;

        do {
            Stat stat;
            bufferSize = _maxDataSize;

            retryController(
                    zoo_wget(_zhandle, path.string().c_str(),
                            &ZKWatcher::watcherFn, watcherContext,
                            &*buffer.begin(),
                            &bufferSize, //in & out
                            &stat));
        } while(retryController.shouldRetry());

        retryController.throwIfError();

        buffer.resize(bufferSize);
        return move(buffer);

    } catch(boost::exception& e) {
        unregisterWatcher(watcherContext);
        e <<errorinfo::Path(path);
        throw;
    }
}

void
ZKFacade::setData(const Path& path, const Buffer& buffer, bool mustExist) {
    return setData(path, &*buffer.begin(), buffer.size(), mustExist);
}

void
ZKFacade::setData(const Path& path, const char* buffer, size_t length, bool mustExist) {
    assert (length < _maxDataSize);

    try {
        if (mustExist || hasNode(path))
            setDataForExistingFile(*this, path, buffer, length, _zhandle);
        else
            setDataForNewFile(*this, path, buffer, length, _zhandle, 0);
    } catch(boost::exception& e) {
        e <<errorinfo::Path(path);
        throw;
    }
}

const Path
ZKFacade::createSequenceNode(const Path& path, const char* buffer, size_t length) {
    assert (length < _maxDataSize);

    int createFlags = ZOO_SEQUENCE;
    return setDataForNewFile(*this, path, buffer, length, _zhandle, createFlags);
}

bool
ZKFacade::hasNode(const Path& path) {
    try {
        RetryController retryController(this);
        do {
            Stat stat;
            const int noWatch = 0;
            retryController(
                    zoo_exists(_zhandle, path.string().c_str(), noWatch, &stat));
        } while(retryController.shouldRetry());

        switch(retryController._lastStatus) {
          case ZNONODE:
            return false;
          case ZOK:
            return true;
          default:
            retryController.throwIfError();
            //this should never happen:
            assert(false);
            return false;
        }

    } catch (boost::exception &e) {
        e <<errorinfo::Path(path);
        throw;
    }
}

bool
ZKFacade::hasNode(const Path& path, const NodeChangedWatcherSP& watcher) {
    void* watcherContext = registerWatcher(watcher);
    try {
        RetryController retryController(this);
        do {
            Stat stat;
            retryController(
                    zoo_wexists(_zhandle, path.string().c_str(),
                            &ZKWatcher::watcherFn, watcherContext,
                            &stat));
        } while(retryController.shouldRetry());

        switch(retryController._lastStatus) {
          case ZNONODE:
            return false;
          case ZOK:
            return true;
          default:
            retryController.throwIfError();
            //this should never happen:
            assert(false);
            return false;
        }

    } catch (boost::exception &e) {
        unregisterWatcher(watcherContext);
        e <<errorinfo::Path(path);
        throw;
    }
}

void
ZKFacade::addEphemeralNode(const Path& path) {
    try {
        setDataForNewFile(*this, path, "", 0, _zhandle, ZOO_EPHEMERAL);
    } catch(const ZKNodeExistsException& e) {
        remove(path);
        addEphemeralNode(path);
    } catch (boost::exception& e) {
        e <<errorinfo::Path(path);
        throw;
    }
}

void
ZKFacade::remove(const Path& path) {
    std::vector< std::string > children = getChildren(path);
    if (!children.empty()) {
        std::for_each(children.begin(), children.end(), [&](const std::string & s){ remove(path / s); });
    }

    try {
        RetryController retryController(this);
        do {
            int ignoreVersion = -1;

            retryController(
                    zoo_delete(_zhandle, path.string().c_str(),
                            ignoreVersion));
        } while (retryController.shouldRetry());

        if (retryController._lastStatus != ZNONODE)
            retryController.throwIfError();

    } catch(boost::exception& e) {
        e <<errorinfo::Path(path);
    }
}

void
ZKFacade::removeIfExists(const Path& path) {
    try {
        if (hasNode(path)) {
            remove(path);
        }
    } catch (const ZKNodeDoesNotExistsException& e) {
        //someone else removed it concurrently, not a problem.
    }
}

void
ZKFacade::retainOnly(const Path& path, const std::vector<std::string>& childrenToPreserve) {
    typedef std::vector<std::string> Children;

    Children current = getChildren(path);
    std::sort(current.begin(), current.end());

    Children toPreserveSorted(childrenToPreserve);
    std::sort(toPreserveSorted.begin(), toPreserveSorted.end());

    std::set_difference(current.begin(), current.end(),
                        toPreserveSorted.begin(), toPreserveSorted.end(),
                        boost::make_function_output_iterator([&](const std::string & s){ remove(path / s); }));
}

std::vector< std::string >
ZKFacade::getChildren(const Path& path) {
    try {
        RetryController retryController(this);
        String_vector children;
        do {
            const bool watch = false;
            retryController(
                    zoo_get_children(_zhandle, path.string().c_str(), watch, &children));
        } while(retryController.shouldRetry());

        retryController.throwIfError();

        DeallocateZKStringVectorGuard deallocateGuard(children);

        typedef std::vector<std::string> ResultType;
        ResultType result;
        result.reserve(children.count);

        std::copy(children.data, children.data + children.count,
                  std::back_inserter(result));

        return result;
    } catch (boost::exception& e) {
        e <<errorinfo::Path(path);
        throw;
    }
}

std::vector< std::string >
ZKFacade::getChildren(const Path& path, const NodeChangedWatcherSP& watcher) {
    void* watcherContext = registerWatcher(watcher);

    try {
        RetryController retryController(this);
        String_vector children;
        do {
            retryController(
                    zoo_wget_children(_zhandle, path.string().c_str(),
                            &ZKWatcher::watcherFn, watcherContext,
                            &children));
        } while(retryController.shouldRetry());

        retryController.throwIfError();

        DeallocateZKStringVectorGuard deallocateGuard(children);

        typedef std::vector<std::string> ResultType;
        ResultType result;
        result.reserve(children.count);

        std::copy(children.data, children.data + children.count,
                  std::back_inserter(result));

        return result;
    } catch (boost::exception& e) {
        unregisterWatcher(watcherContext);
        e <<errorinfo::Path(path);
        throw;
    }
}


void
ZKFacade::disableRetries() {
    _retriesEnabled = false;
}

ZKLogging::ZKLogging() :
    _file(nullptr)
{
    std::string filename(vespa::Defaults::vespaHome());
    filename.append("/tmp/zookeeper.log");
    _file = std::fopen(filename.c_str(), "w");
    if (_file == nullptr) {
         std::cerr <<"Could not open file " <<filename << std::endl;
    } else {
         zoo_set_log_stream(_file);
    }

    zoo_set_debug_level(ZOO_LOG_LEVEL_ERROR);
}

ZKLogging::~ZKLogging()
{
    zoo_set_log_stream(nullptr);
    if (_file != nullptr) {
        std::fclose(_file);
        _file = nullptr;
    }
}

const char*
ZKGenericException::what() const throw() {
    switch(_zkStatus) {
      //System errors
      case ZRUNTIMEINCONSISTENCY:
        return "Zookeeper: A runtime inconsistency was found(ZRUNTIMEINCONSISTENCY)";
      case ZDATAINCONSISTENCY:
        return "Zookeeper: A data inconsistency was found(ZDATAINCONSISTENCY)";
      case ZCONNECTIONLOSS:
        return "Zookeeper: Connection to the server has been lost(ZCONNECTIONLOSS)";
      case ZMARSHALLINGERROR:
        return "Zookeeper: Error while marshalling or unmarshalling data(ZMARSHALLINGERROR)";
      case ZUNIMPLEMENTED:
        return "Zookeeper: Operation is unimplemented(ZUNIMPLEMENTED)";
      case ZOPERATIONTIMEOUT:
        return "Zookeeper: Operation timeout(ZOPERATIONTIMEOUT)";
      case ZBADARGUMENTS:
        return "Zookeeper: Invalid arguments(ZBADARGUMENTS)";
      case ZINVALIDSTATE:
        return "Zookeeper: The connection with the zookeeper servers timed out(ZINVALIDSTATE).";

      //API errors
      case ZNONODE:
        return "Zookeeper: Node does not exist(ZNONODE)";
      case ZNOAUTH:
        return "Zookeeper: Not authenticated(ZNOAUTH)";
      case ZBADVERSION:
        return "Zookeeper: Version conflict(ZBADVERSION)";
      case ZNOCHILDRENFOREPHEMERALS:
        return "Zookeeper: Ephemeral nodes may not have children(ZNOCHILDRENFOREPHEMERALS)";
      case ZNODEEXISTS:
        return "Zookeeper: The node already exists(ZNODEEXISTS)";
      case ZNOTEMPTY:
        return "Zookeeper: The node has children(ZNOTEMPTY)";
      case ZSESSIONEXPIRED:
        return "Zookeeper: The session has been expired by the server(ZSESSIONEXPIRED)";
      case ZINVALIDCALLBACK:
        return "Zookeeper: Invalid callback specified(ZINVALIDCALLBACK)";
      case ZINVALIDACL:
        return "Zookeeper: Invalid ACL specified(ZINVALIDACL)";
      case ZAUTHFAILED:
        return "Zookeeper: Client authentication failed(ZAUTHFAILED)";
      case ZCLOSING:
        return "Zookeeper: ZooKeeper is closing(ZCLOSING)";
      case ZNOTHING:
        return "Zookeeper: No server responses to process(ZNOTHING)";
      default:
        std::cerr <<"In ZKGenericException::what(): Invalid error code " <<_zkStatus <<std::endl;
        return "Zookeeper: Invalid error code.";
    }
}

const std::string
filedistribution::diagnosticUserLevelMessage(const ZKException& exception) {
    const char* indent = "    ";
    std::ostringstream message;
    message <<exception.what();

    const errorinfo::Path::value_type* path = boost::get_error_info<errorinfo::Path>(exception);
    if (path) {
        message <<std::endl <<indent <<"Path: " <<*path;
    }
    return message.str();
}
