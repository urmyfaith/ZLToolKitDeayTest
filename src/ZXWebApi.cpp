#include "jsoncpp/json.h"
#include <sys/stat.h>
#include <math.h>
#include <signal.h>
#include <functional>
#include <sstream>
#include <unordered_map>

#include "Util/util.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"

#include "Common/config.h"
#include "Http/HttpRequester.h"
#include "Http/HttpSession.h"
#include "Network/TcpServer.h"
#include "Thread/WorkThreadPool.h"

#include "Util/MD5.h"
#include "ZXWebApi.h"



using namespace Json;
using namespace toolkit;

namespace API {
typedef enum {
	MethodNotMatch = -5,//请求方式不匹配
    Exception = -4,//代码抛异常
    InvalidArgs = -3,//参数不合法
    SqlFailed = -2,//sql执行失败
    AuthFailed = -1,//鉴权失败
    OtherFailed = -1,//业务代码执行失败，
    Success = 0//执行成功
} ApiErr;

#define API_FIELD "api."
const string kApiDebug = API_FIELD"apiDebug";
const string kSecret = API_FIELD"secret";
const string kSnapRoot = API_FIELD"snapRoot";
const string kDefaultSnap = API_FIELD"defaultSnap";

static onceToken token([]() {
    mINI::Instance()[kApiDebug] = "1";
    mINI::Instance()[kSecret] = "035c73f7-bb6b-4889-a715-d9eb2d1925cc";
    mINI::Instance()[kSnapRoot] = "./www/snap/";
    mINI::Instance()[kDefaultSnap] = "./www/logo.png";
});
}//namespace API


class ApiRetException: public std::runtime_error {
public:
    ApiRetException(const char *str = "success" ,int code = API::Success):runtime_error(str){
        _code = code;
    }
    ~ApiRetException() = default;
    int code(){ return _code; }
private:
    int _code;
};

class AuthException : public ApiRetException {
public:
    AuthException(const char *str):ApiRetException(str,API::AuthFailed){}
    ~AuthException() = default;
};

class InvalidArgsException: public ApiRetException {
public:
    InvalidArgsException(const char *str):ApiRetException(str,API::InvalidArgs){}
    ~InvalidArgsException() = default;
};

class SuccessException: public ApiRetException {
public:
    SuccessException():ApiRetException("success",API::Success){}
    ~SuccessException() = default;
};

#define API_ARGS1 SockInfo &sender,const Parser &parser,HttpSession::KeyValue &headerIn, HttpSession::KeyValue &headerOut, ApiArgsType &allArgs, Json::Value &val
#define API_ARGS2 API_ARGS1, const HttpSession::HttpResponseInvoker &invoker
#define API_ARGS_VALUE1 sender,parser,headerIn,headerOut,allArgs,val
#define API_ARGS_VALUE2 API_ARGS_VALUE1, invoker

typedef map<string, variant, StrCaseCompare> ApiArgsType;
//http api列表
static map<string, std::function<void(API_ARGS2)> > s_map_api;

template<typename FUNC>
static void api_regist1(const string &api_path, FUNC &&func) {
    s_map_api.emplace(api_path, [func](API_ARGS2) {
        func(API_ARGS_VALUE1);
        invoker("200 OK", headerOut, val.toStyledString());
    });
}

template<typename FUNC>
static void api_regist2(const string &api_path, FUNC &&func) {
    s_map_api.emplace(api_path, std::forward<FUNC>(func));
}

//获取HTTP请求中url参数、content参数
static ApiArgsType getAllArgs(const Parser &parser) {
    ApiArgsType allArgs;
    if (parser["Content-Type"].find("application/x-www-form-urlencoded") == 0) {
        auto contentArgs = parser.parseArgs(parser.Content());
        for (auto &pr : contentArgs) {
            allArgs[pr.first] = HttpSession::urlDecode(pr.second);
        }
    } else if (parser["Content-Type"].find("application/json") == 0) {
        try {
            stringstream ss(parser.Content());
            Value jsonArgs;
            ss >> jsonArgs;
            auto keys = jsonArgs.getMemberNames();
            for (auto key = keys.begin(); key != keys.end(); ++key) {
                allArgs[*key] = jsonArgs[*key].asString();
            }
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    } else if (!parser["Content-Type"].empty()) {
        WarnL << "invalid Content-Type:" << parser["Content-Type"];
    }

    for (auto &pr :  parser.getUrlArgs()) {
        allArgs[pr.first] = pr.second;
    }
    return std::move(allArgs);
}

static inline void addHttpListener(){
    GET_CONFIG(bool, api_debug, API::kApiDebug);
    //注册监听kBroadcastHttpRequest事件
    NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastHttpRequest, [](BroadcastHttpRequestArgs) {
        auto it = s_map_api.find(parser.Url());
        if (it == s_map_api.end()) {
            return;
        }
        //该api已被消费
        consumed = true;
        //执行API
        Json::Value val;
        val["code"] = API::Success;
        HttpSession::KeyValue headerOut;
        auto allArgs = getAllArgs(parser);
        HttpSession::KeyValue &headerIn = parser.getHeader();
        GET_CONFIG(string,charSet,Http::kCharSet);
        headerOut["Content-Type"] = StrPrinter << "application/json; charset=" << charSet;
        if(api_debug){
            auto newInvoker = [invoker,parser,allArgs](const string &codeOut,
                                                       const HttpSession::KeyValue &headerOut,
                                                       const HttpBody::Ptr &body){
                stringstream ss;
                for(auto &pr : allArgs ){
                    ss << pr.first << " : " << pr.second << "\r\n";
                }

                //body默认为空
                int64_t size = 0;
                if (body && body->remainSize()) {
                    //有body，获取body大小
                    size = body->remainSize();
                }

                if(size && size < 4 * 1024){
                    string contentOut = body->readData(size)->toString();
                    DebugL << "\r\n# request:\r\n" << parser.Method() << " " << parser.FullUrl() << "\r\n"
                           << "# content:\r\n" << parser.Content() << "\r\n"
                           << "# args:\r\n" << ss.str()
                           << "# response:\r\n"
                           << contentOut << "\r\n";
                    invoker(codeOut,headerOut,contentOut);
                } else{
                    DebugL << "\r\n# request:\r\n" << parser.Method() << " " << parser.FullUrl() << "\r\n"
                           << "# content:\r\n" << parser.Content() << "\r\n"
                           << "# args:\r\n" << ss.str()
                           << "# response size:"
                           << size <<"\r\n";
                    invoker(codeOut,headerOut,body);
                }
            };
            ((HttpSession::HttpResponseInvoker &)invoker) = newInvoker;
        }

        try {
            it->second(sender,parser,headerIn, headerOut, allArgs, val, invoker);
        }  catch(ApiRetException &ex){
            val["code"] = ex.code();
            val["msg"] = ex.what();
            invoker("200 OK", headerOut, val.toStyledString());
        }
        catch (std::exception &ex) {
            val["code"] = API::Exception;
            val["msg"] = ex.what();
            invoker("200 OK", headerOut, val.toStyledString());
        }
    });
}

template <typename Args,typename First>
bool checkArgs(Args &&args,First &&first){
    return !args[first].empty();
}

template <typename Args,typename First,typename ...KeyTypes>
bool checkArgs(Args &&args,First &&first,KeyTypes && ...keys){
    return !args[first].empty() && checkArgs(std::forward<Args>(args),std::forward<KeyTypes>(keys)...);
}

#define CHECK_ARGS(...)  \
    if(!checkArgs(allArgs,##__VA_ARGS__)){ \
        throw InvalidArgsException("缺少必要参数:" #__VA_ARGS__); \
    }

#define CHECK_SECRET() \
    if(sender.get_peer_ip() != "127.0.0.1"){ \
        CHECK_ARGS("secret"); \
        if(api_secret != allArgs["secret"]){ \
            throw AuthException("secret错误"); \
        } \
    }


static inline string getProxyKey(const string &vhost,const string &app,const string &stream){
    return vhost + "/" + app + "/" + stream;
}

/**
 * 安装api接口
 * 所有api都支持GET和POST两种方式
 * POST方式参数支持application/json和application/x-www-form-urlencoded方式
 */
void zxinstallWebApi() {
	addHttpListener();
	GET_CONFIG(string, api_secret, API::kSecret);

	static auto s_get_api_list = [](API_ARGS1) {
		CHECK_SECRET();
		for (auto &pr : s_map_api) {
			val["data"].append(pr.first);
		}
	};

	//获取服务器api列表
	//测试url http://127.0.0.1/index/api
	api_regist1("/index/api", [](API_ARGS1) {
		s_get_api_list(API_ARGS_VALUE1);
	});



	typedef std::function<void(Json::Value &device)> snapCallback;

	api_regist1("/test1", [](API_ARGS1) {
		semaphore sem;

		/*
		if (parser.Method() == "GET") {
			throw MethodException("please use POST!");
		}
		*/

		//CHECK_SECRET();
		//CHECK_ARGS("device_id");
		std::string device_id = "123";
		bool result = false;
	
		int maxCount = 3;
		std::mutex  snap_map_mux;
		for (int i =0; i< maxCount; i++)
		{
			if (i == 1)
			{

				DebugL << "[test] start\t" << device_id << endl;
				snap_map_mux.lock();
				int delay = 3000;

				DebugL << "[test] start delay; when timeout, clean callback." << endl;
			    auto tag = EventPollerPool::Instance().getPoller()->doDelayTask(delay, [&]() {
					WarnL << "[test] done TimeOut\t" << device_id << endl;
					WarnL << "[test] remove cb from map....\t" << device_id << endl;
					result = false;
					sem.post();
					return 0;
				});

				snapCallback cb = [&sem, &val, &result,&device_id,&tag](Json::Value &device) {
					tag->cancel();
					val["data"]["device"] = device;
					result = true;
					DebugL << "[test] done OK\t"  << device_id <<  endl;
					sem.post();
				};
				snap_map_mux.unlock();

				DebugL << "[test] save cb into map..."  << endl;
				break;
			}
		}
		sem.wait();
		val["result"] = result;
		val["code"] = result ? API::Success : API::OtherFailed;
		val["msg"] = result ? "success" : "snap failed";
	});

	api_regist1("/test2", [](API_ARGS1) {
		semaphore sem;
		bool result = false;
		DebugL << "[test] A" << endl;
		auto tag = EventPollerPool::Instance().getPoller()->doDelayTask(3000, [&]() {
			WarnL << "[snapbyhand] done TimeOut\t" << endl;
			sem.post();
			return 0;
		});
		sem.wait();
		DebugL << "[test] B" << endl;
		val["result"] = result;
		val["code"] = result ? API::Success : API::OtherFailed;
		val["msg"] = result ? "success" : "exe failed";
	});

	static auto checkAccess = [](const string &params) {
		//我们假定大家都要权限访问
		return true;
	};
}

void zxunInstallWebApi(){


}