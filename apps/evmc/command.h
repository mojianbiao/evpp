#pragma once

#include "mctypes.h"
#include <glog/logging.h>

namespace evmc {

class Command  : public std::enable_shared_from_this<Command> {
public:
    Command(evpp::EventLoop* evloop, uint16_t vbucket, uint32_t th_hash)
        : caller_loop_(evloop), id_(0)
        , vbucket_id_(vbucket), thread_hash_(th_hash){
			buf_.reserve(512);
    }
    void Init(evpp::EventLoop* evloop, uint16_t vbucket, uint32_t th_hash) {
		caller_loop_ = evloop;
		vbucket_id_ = vbucket;
		thread_hash_ = th_hash; 
		buf_.clear();
	}

    virtual ~Command() {}

    uint32_t id() const {
        return id_;
    }
    void set_id(uint32_t v) {
        id_ = v;
    }
    uint32_t thread_hash() const {
        return thread_hash_;
    }
    uint16_t vbucket_id() const {
        return vbucket_id_;
    }

    uint16_t server_id() const;
    void set_server_id(uint16_t sid) {
        server_id_history_.push_back(sid);
    }

    evpp::EventLoop* caller_loop() const {
        return caller_loop_;
    }

    void Launch(MemcacheClientPtr memc_client);

    bool ShouldRetry() const;

    virtual void OnError(int code) = 0;
    virtual void OnSetCommandDone(int resp_code) {}
    virtual void OnRemoveCommandDone(int resp_code) {}
    virtual void OnGetCommandDone(int resp_code, const std::string& value) {}
    virtual void OnMultiGetCommandOneResponse(int resp_code, std::string& key, std::string& value) {}
    virtual void OnMultiGetCommandDone(int resp_code, std::string& key, std::string& value) {}
    virtual void OnPrefixGetCommandDone(const int resp_code, std::string& key) {}
	virtual void OnPrefixGetCommandOneResponse(std::string& key, std::string& value) {}
	virtual bool IsDone() {return true;}

private:
    virtual BufferPtr RequestBuffer() = 0;
    evpp::EventLoop* caller_loop_;
    uint32_t id_;               // 并非全局id，只是各个memc_client内部的序号; mget的多个命令公用一个id
    uint16_t vbucket_id_;
    std::vector<uint16_t> server_id_history_;        // 执行时从多个备选server中所选定的server
    uint32_t thread_hash_;
protected:
	std::string buf_;
};
typedef std::shared_ptr<Command> CommandPtr;

class SetCommand  : public Command {
public:
    SetCommand(evpp::EventLoop* evloop, uint16_t vbucket, const std::string& key, const std::string& value,
               uint32_t flags, uint32_t expire, SetCallback callback)
        : Command(evloop, vbucket, next_thread_++), key_(key), value_(value),
          flags_(flags), expire_(expire),
          set_callback_(callback) {
    }

    virtual void OnError(int err_code) {
        LOG_INFO << "SetCommand OnError id=" << id();

        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(set_callback_,
                                               key_, err_code));
        } else {
            set_callback_(key_, err_code);
        }
    }
    virtual void OnSetCommandDone(int resp_code) {
        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(set_callback_, key_, resp_code));
        } else {
            set_callback_(key_, resp_code);
        }
    }
private:
    std::string key_;
    std::string value_;
    uint32_t flags_;
    uint32_t expire_;
    SetCallback set_callback_;
    static std::atomic_int next_thread_;
private:
    virtual BufferPtr RequestBuffer() ;
};

class GetCommand : public Command {
public:
    GetCommand(evpp::EventLoop* evloop, uint16_t vbucket, const std::string& key, GetCallback callback)
        : Command(evloop, vbucket, next_thread_++)
        , key_(key)
        , get_callback_(callback) {
    }

    virtual void OnError(int err_code) {
        LOG(WARNING) << "GetCommand OnError id=" << id();

        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(get_callback_, key_,
                                               GetResult(err_code, std::string())));
        } else {
            get_callback_(key_, GetResult(err_code, std::string()));
        }
    }
    virtual void OnGetCommandDone(int resp_code, const std::string& value) {
        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(get_callback_, key_,
                                               GetResult(resp_code, value)));
        } else {
            get_callback_(key_, GetResult(resp_code, value));
        }
    }
private:
    std::string key_;
    GetCallback get_callback_;
    static std::atomic_int next_thread_;
private:
    virtual BufferPtr RequestBuffer() ;
};

class PrefixGetCommand  : public Command {
public:
    PrefixGetCommand(evpp::EventLoop* evloop, uint16_t vbucket, const std::string& key, PrefixGetCallback callback)
        : Command(evloop, vbucket, next_thread_++), key_(key), mget_callback_(callback), mget_result_(new PrefixGetResult()) {
    }

    virtual void OnError(int err_code) {
        LOG(WARNING) << "PrefixGetCommand OnError id=" << id();
        mget_result_->code = err_code;

        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(mget_callback_, key_, mget_result_));
        } else {
            mget_callback_(key_, mget_result_);
        }
    }
    virtual void OnPrefixGetCommandDone(const int resp_code, std::string& key);
	virtual void OnPrefixGetCommandOneResponse(std::string& key, std::string& value);
private:
    std::string key_;
    PrefixGetCallback mget_callback_;
    PrefixGetResultPtr mget_result_;
    static std::atomic_int next_thread_;
private:
    virtual BufferPtr RequestBuffer();
};

class PrefixMultiGetCommand  : public Command {
public:
    PrefixMultiGetCommand(evpp::EventLoop* evloop, uint16_t vbucket, uint32_t th_hash, const std::vector<std::string>& keys, PrefixMultiGetCallback callback)
        : Command(evloop, vbucket, th_hash), keys_(keys), mget_callback_(callback), mget_all_prefix_result_(new PrefixMultiGetResult()), is_done_(false) {
    }

    virtual void OnError(int err_code) {
        LOG(WARNING) << "prefixMultiGetCommand OnError id=" << id();
        mget_all_prefix_result_->code = err_code;
		/*PrefixGetResult res;
		res.code = err_code;
		auto & result_map = mget_all_prefix_result_.get_result_map_;
		for (auto it = keys_.begin(); it != keys_.end(); ++it) {
			if (result_map.find(*it) == result_map.end()) {
				result_map.insert(std::make_pair(*it, res));
			}
		}
		if (keys_.size() != result_map.size()) {
			LOG(WARNING) << "recv some unexpected key info";
		}*/

        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(mget_callback_, mget_all_prefix_result_));
        } else {
			mget_callback_(mget_all_prefix_result_);
        }
    }
    virtual void OnPrefixGetCommandDone(const int resp_code, std::string& key);
	virtual void OnPrefixGetCommandOneResponse(std::string& key, std::string& value);
	virtual bool IsDone() { return is_done_; } 
private:
    std::vector<std::string> keys_;
    PrefixMultiGetCallback mget_callback_;
    PrefixGetResult mget_result_;
    PrefixMultiGetResultPtr mget_all_prefix_result_;
	bool is_done_;
private:
    virtual BufferPtr RequestBuffer();
};

class MultiGetCommand  : public Command {
public:
    MultiGetCommand(evpp::EventLoop* evloop, uint16_t vbucket, uint32_t th_hash, const std::vector<std::string>& keys, MultiGetCallback callback)
        : Command(evloop, vbucket, th_hash), keys_(keys), mget_callback_(callback) {
    }

    virtual void OnError(int err_code) {
        LOG(WARNING) << "MultiGetCommand OnError id=" << id();
        mget_result_.code = err_code;
		auto & result_map = mget_result_.get_result_map_;
        for (auto it = keys_.begin(); it != keys_.end(); ++it) {
			if (result_map.find(*it) == result_map.end()) {
				result_map.emplace(*it, GetResult(err_code, "not enough connections"));
			}
		}
		if (keys_.size() != result_map.size()) {
			LOG(WARNING) << "recv some unrequested key info";
		}

        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(mget_callback_, mget_result_));
        } else {
            mget_callback_(mget_result_);
        }
    }
    virtual void OnMultiGetCommandDone(int resp_code, std::string& key, std::string& value);
    virtual void OnMultiGetCommandOneResponse(int resp_code, std::string& key, std::string& value);
private:
    std::vector<std::string> keys_;
    MultiGetCallback mget_callback_;
    MultiGetResult mget_result_;
private:
    virtual BufferPtr RequestBuffer();
};

class MultiGetCommand2 : public Command {
public:
    MultiGetCommand2()
        : Command(NULL, 0, 0){
    }
	
    void Init(evpp::EventLoop* evloop, uint16_t vbucket, uint32_t th_hash, std::vector<std::string>& keys,MultiGetCollector2Ptr collector) {
		keys_ = std::move(keys);
		collector_ = collector;
		Command::Init(evloop, vbucket, th_hash);
    }

    virtual void OnError(int err_code) {
        LOG(WARNING) << "MultiGetCommand OnError id=" << id();
		for (auto it = keys_.begin(); it != keys_.end(); ++it) {
			GetResult gt(err_code, std::string("get failed"));
			collector_->Collect(*it, gt);
		}
//		MultiGetMapResultPtr result = std::make_shared<MultiGetMapResult >(std::move(mget_result_));
//
//        if (caller_loop()) {
//            caller_loop()->RunInLoop(std::bind(mget_callback_, result, err_code));
//        } else {
//           mget_callback_(result, err_code);
//        }
    }
    virtual void OnMultiGetCommandDone(int resp_code, std::string& key, std::string& value);
    virtual void OnMultiGetCommandOneResponse(int resp_code, std::string& key, std::string& value);
private:
    std::vector<std::string> keys_;
	MultiGetCollector2Ptr collector_;
private:
    virtual BufferPtr RequestBuffer();
};

class RemoveCommand  : public Command {
public:
    RemoveCommand(evpp::EventLoop* evloop, uint16_t vbucket, const std::string& key, RemoveCallback callback)
        : Command(evloop, vbucket, next_thread_++), key_(key), remove_callback_(callback) {
    }
    virtual void OnError(int err_code) {
        LOG(WARNING) << "RemoveCommand OnError id=" << id();

        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(remove_callback_, key_, err_code));
        } else {
            remove_callback_(key_, err_code);
        }
    }
    virtual void OnRemoveCommandDone(int resp_code) {
        if (caller_loop()) {
            caller_loop()->RunInLoop(std::bind(remove_callback_, key_, resp_code));
        } else {
            remove_callback_(key_, resp_code);
        }
    }
private:
    std::string key_;
    RemoveCallback remove_callback_;
    static std::atomic_int next_thread_;
private:
    virtual BufferPtr RequestBuffer();
};

}

