#include <map>
#include <signal.h>
#include <iostream>
#include "Util/File.h"
#include "Util/SSLBox.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Common/config.h"
#include "Http/WebSocketSession.h"
#include "ZXWebApi.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;


int main(int argc,char *argv[]){
   
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); });// 设置退出信号

    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

	zxinstallWebApi();

	//启动http服务器
	TcpServer::Ptr httpSrv(new TcpServer());

	int http_port = std::stoi("22222");
	try {
		if (http_port) {
			httpSrv->start<HttpSession>(http_port);
		}
	}
	catch (std::exception &ex) {
		WarnL << "Port error" << ex.what() << endl;
		ErrorL << "Please use another port!" << endl;
		sleep(1);
		return -1;
	}
	InfoL << "started http api at port\t" << std::to_string(http_port);
    InfoL << "http://127.0.0.1:22222/test1" << endl;
	InfoL << "http://127.0.0.1:22222/test2" << endl;

    sem.wait();
    return 0;
}

