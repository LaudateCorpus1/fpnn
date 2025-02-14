#include <exception>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "ClientEngine.h"
#include "UDPClient.h"
#include "ConnectionReclaimer.h"
#include "FPWriter.h"
#include "AutoRelease.h"
#include "FileSystemUtil.h"

using namespace fpnn;

UDPClient::UDPClient(const std::string& host, int port, bool autoReconnect): Client(host, port, autoReconnect) {}

class UDPQuestTask: public ITaskThreadPool::ITask
{
	FPQuestPtr _quest;
	ConnectionInfoPtr _connectionInfo;
	UDPClientPtr _client;

public:
	UDPQuestTask(UDPClientPtr client, FPQuestPtr quest, ConnectionInfoPtr connectionInfo):
		_quest(quest), _connectionInfo(connectionInfo), _client(client) {}

	virtual ~UDPQuestTask() {}

	virtual void run()
	{
		try
		{
			_client->processQuest(_quest, _connectionInfo);
		}
		catch (const FpnnError& ex){
			LOG_ERROR("UDP client processQuest() error:(%d)%s. %s", ex.code(), ex.what(), _connectionInfo->str().c_str());
		}
		catch (...)
		{
			LOG_ERROR("Fatal error occurred when UDP client processQuest() function. %s", _connectionInfo->str().c_str());
		}
	}
};

void UDPClient::dealQuest(FPQuestPtr quest, ConnectionInfoPtr connectionInfo)		//-- must done in thread pool or other thread.
{
	if (!_questProcessor)
	{
		LOG_ERROR("Recv a quest but UDP client without quest processor. %s", connectionInfo->str().c_str());
		return;
	}

	bool wakeup, exiting;
	std::shared_ptr<UDPQuestTask> task(new UDPQuestTask(shared_from_this(), quest, connectionInfo));
	if (_questProcessPool)
	{
		wakeup = _questProcessPool->wakeUp(task);
		if (!wakeup) exiting = _questProcessPool->exiting();
	}
	else
	{
		wakeup = ClientEngine::wakeUpQuestProcessThreadPool(task);
		if (!wakeup) exiting = ClientEngine::questProcessPoolExiting();
	}

	if (!wakeup)
	{
		if (exiting)
		{
			LOG_ERROR("wake up thread pool to process UDP client quest failed. Quest pool is exiting. %s", connectionInfo->str().c_str());
		}
		else
		{
			LOG_ERROR("wake up thread pool to process UDP client quest failed. Quest pool limitation is caught. Quest task havn't be executed. %s",
				connectionInfo->str().c_str());

			if (quest->isTwoWay())
			{
				try
				{
					FPAnswerPtr answer = FpnnErrorAnswer(quest, FPNN_EC_CORE_WORK_QUEUE_FULL, std::string("worker queue full, ") + connectionInfo->str().c_str());
					std::string *raw = answer->raw();
					_engine->sendData(connectionInfo->socket, connectionInfo->token, raw);
				}
				catch (const FpnnError& ex)
				{
					LOG_ERROR("Generate error answer for UDP duplex client worker queue full failed. No answer returned, peer need to wait timeout. %s, exception:(%d)%s",
						connectionInfo->str().c_str(), ex.code(), ex.what());
				}
				catch (...)
				{
					LOG_ERROR("Generate error answer for UDP duplex client worker queue full failed. No answer returned, peer need to wait timeout. %s",
						connectionInfo->str().c_str());
				}
			}
		}
	}
}

bool UDPClient::perpareConnection(ConnectionInfoPtr currConnInfo)
{
	UDPClientConnection* connection = new UDPClientConnection(shared_from_this(), &_mutex, currConnInfo);

	connected(connection);

	bool joined = ClientEngine::nakedInstance()->joinEpoll(connection);
	if (!joined)
	{
		LOG_ERROR("Join epoll failed after UDP connected event. %s", currConnInfo->str().c_str());
		errorAndWillBeClosed(connection);
		return false;
	}
	
	return true;
}

int UDPClient::connectIPv4Address(ConnectionInfoPtr currConnInfo)
{
	int socketfd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (socketfd < 0)
		return 0;

	size_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in* serverAddr = (struct sockaddr_in*)malloc(addrlen);

	memset(serverAddr, 0, addrlen);
	serverAddr->sin_family = AF_INET;
	serverAddr->sin_addr.s_addr = inet_addr(currConnInfo->ip.c_str()); 
	serverAddr->sin_port = htons(currConnInfo->port);

	if (serverAddr->sin_addr.s_addr == INADDR_NONE)
	{
		::close(socketfd);
		free(serverAddr);
		return 0;
	}

	if (::connect(socketfd, (struct sockaddr *)serverAddr, addrlen) != 0)
	{
		::close(socketfd);
		free(serverAddr);
		return 0;
	}

	currConnInfo->changToUDP(socketfd, (uint8_t*)serverAddr);

	return socketfd;
}
int UDPClient::connectIPv6Address(ConnectionInfoPtr currConnInfo)
{
	int socketfd = ::socket(AF_INET6, SOCK_DGRAM, 0);
	if (socketfd < 0)
		return 0;

	size_t addrlen = sizeof(struct sockaddr_in6);
	struct sockaddr_in6* serverAddr = (struct sockaddr_in6*)malloc(addrlen);

	memset(serverAddr, 0, addrlen);
	serverAddr->sin6_family = AF_INET6;  
	serverAddr->sin6_port = htons(currConnInfo->port);

	if (inet_pton(AF_INET6, currConnInfo->ip.c_str(), &serverAddr->sin6_addr) != 1)
	{
		::close(socketfd);
		free(serverAddr);
		return 0;
	}

	if (::connect(socketfd, (struct sockaddr *)serverAddr, addrlen) != 0)
	{
		::close(socketfd);
		free(serverAddr);
		return 0;
	}

	currConnInfo->changToUDP(socketfd, (uint8_t*)serverAddr);

	return socketfd;
}

bool UDPClient::connect()
{
	if (_connected)
		return true;

	ConnectionInfoPtr currConnInfo;
	{
		std::unique_lock<std::mutex> lck(_mutex);
		while (_connStatus == ConnStatus::Connecting)
			_condition.wait(lck);

		if (_connStatus == ConnStatus::Connected)
			return true;

		currConnInfo = _connectionInfo;

		_connected = false;
		_connStatus = ConnStatus::Connecting;
	}

	UDPClient* self = this;

	CannelableFinallyGuard errorGuard([self, currConnInfo](){
		std::unique_lock<std::mutex> lck(self->_mutex);
		if (currConnInfo.get() == self->_connectionInfo.get())
		{
			if (self->_connectionInfo->socket)
			{
				ConnectionInfoPtr newConnectionInfo(new ConnectionInfo(0, self->_connectionInfo->port, self->_connectionInfo->ip, self->_isIPv4, false));
				self->_connectionInfo = newConnectionInfo;
			}

			self->_connected = false;
			self->_connStatus = ConnStatus::NoConnected;
		}
		self->_condition.notify_all();
	});

	int socket = 0;
	if (_isIPv4)
		socket = connectIPv4Address(currConnInfo);
	else
		socket = connectIPv6Address(currConnInfo);

	if (socket == 0)
	{
		LOG_ERROR("UDP client connect remote server %s failed.", currConnInfo->str().c_str());
		return false;
	}

	if (!perpareConnection(currConnInfo))
		return false;

	errorGuard.cancel();
	{
		std::unique_lock<std::mutex> lck(_mutex);
		if (_connectionInfo.get() == currConnInfo.get())
		{
			_connected = true;
			_connStatus = ConnStatus::Connected;
			_condition.notify_all();

			return true;
		}
	}

	LOG_ERROR("This codes (UDPClient::connect dupled) is impossible touched. This is just a safety inspection. If this ERROR triggered, please tell swxlion to fix it.");

	//-- dupled
	UDPClientConnection* conn = (UDPClientConnection*)_engine->takeConnection(currConnInfo.get());
	if (conn)
	{
		_engine->exitEpoll(conn);
		clearConnectionQuestCallbacks(conn, FPNN_EC_CORE_CONNECTION_CLOSED);
		willClose(conn);
	}

	std::unique_lock<std::mutex> lck(_mutex);

	while (_connStatus == ConnStatus::Connecting)
		_condition.wait(lck);

	_condition.notify_all();
	if (_connStatus == ConnStatus::Connected)
		return true;

	return false;
}

UDPClientPtr Client::createUDPClient(const std::string& host, int port, bool autoReconnect)
{
	return UDPClient::createClient(host, port, autoReconnect);
}
UDPClientPtr Client::createUDPClient(const std::string& endpoint, bool autoReconnect)
{
	return UDPClient::createClient(endpoint, autoReconnect);
}
