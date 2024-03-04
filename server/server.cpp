// updater.cpp: определяет точку входа для приложения.
//

#include "server.h"
#include "Watchdog.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <filesystem>
#include <functional>
#include <fstream>
#include "json.hpp"
#include "spdlog/spdlog.h"
#include <sstream>
using namespace std;
namespace fs = std::filesystem;
class ServerUpdate
{
public:
    ServerUpdate(const std::string& dir_path_);
    ~ServerUpdate();
    void MessageCallback(std::shared_ptr<ix::ConnectionState> connectionState, ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg);
    void StartUpdate();
private:
    void HashFS();
    size_t OneHash();
    std::tuple<std::size_t, std::vector<uint8_t>> ServerUpdate::hash_file(const std::string& filepath);
    std::size_t one_hash{ 0 };
    std::unique_ptr< ix::WebSocketServer> server;
    std::atomic_bool                                            need_update{ false };
    std::string dir_path;
    std::map<std::string, std::tuple<std::size_t,std::vector<uint8_t>>> file_hash;

};
void ServerUpdate::StartUpdate()
{
    one_hash = OneHash();
    wd::watch(fs::path(dir_path),
        [&](const fs::path& path) { 
            SPDLOG_INFO("Server files update");
            need_update.store(true, std::memory_order_seq_cst); });
    
}
void ServerUpdate::HashFS()
{

    for (const auto& entry : fs::recursive_directory_iterator(dir_path))
        if (entry.is_regular_file())
        {
            fs::path relative_path = entry.path().lexically_relative(dir_path);
            file_hash.emplace(relative_path.u8string(), hash_file(entry.path().u8string()));


        }
}
size_t ServerUpdate::OneHash()
{
    HashFS();
    std::size_t result = 0;

    // Сливаем хэши отсортированных элементов вектора
    for (auto&& [file, pair] : file_hash) {
        // Вычисляем новый хэш как XOR текущего результата и хэша элемента
        auto [hash, data] = pair;
        result ^= hash;
    }
    return result;
}
std::tuple<std::size_t, std::vector<uint8_t>> ServerUpdate::hash_file(const std::string& filepath) {
    // Открываем файл для чтения в бинарном режиме
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        SPDLOG_ERROR("Failed to open file:{} ", filepath);
        return std::make_tuple(0,std::vector<uint8_t>()); // Возврат 0 в случае ошибки
    }

    // Создаем объект хэш-функции
    std::hash<std::string> hasher;

    // Читаем содержимое файла и вычисляем хэш
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> content_vec(content.begin(), content.end());
    std::size_t file_hash = hasher(content);
    file.close();
    return std::make_tuple(file_hash, content_vec);
}
void ServerUpdate::MessageCallback(std::shared_ptr<ix::ConnectionState> connectionState, ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg)
{
    // The ConnectionState object contains information about the connection,
      // at this point only the client ip address and the port.
   
    SPDLOG_INFO("Remote ip: {}", connectionState->getRemoteIp());
    if (msg->type == ix::WebSocketMessageType::Open)
    {
        std::stringstream ss;
        ss<< "New connection" << std::endl;

        // A connection state object is available, and has a default id
        // You can subclass ConnectionState and pass an alternate factory
        // to override it. It is useful if you want to store custom
        // attributes per connection (authenticated bool flag, attributes, etc...)
       ss << "id: " << connectionState->getId() << std::endl;

        // The uri the client did connect to.
        ss << "Uri: " << msg->openInfo.uri << std::endl;

        ss << "Headers:" << std::endl;
        for (auto it : msg->openInfo.headers)
        {
            ss << "\t" << it.first << ": " << it.second << std::endl;
        }
        SPDLOG_INFO(ss.str());
    }
    else if (msg->type == ix::WebSocketMessageType::Message)
    {
        // For an echo server, we just send back to the client whatever was received by the server
        // All connected clients are available in an std::set. See the broadcast cpp example.
        // Second parameter tells whether we are sending the message in binary or text mode.
        // Here we send it in the same mode as it was received.
        if(need_update.load()==true)
            one_hash = OneHash();
        if (!msg->str.empty())
        {
            nlohmann::json pack=nlohmann::json::from_cbor(msg->str);
            if (pack["command"]=="get_hash" && pack.count("hash"))
            {
                if (pack["hash"].get<size_t>() == one_hash)
                {
                    SPDLOG_TRACE("Equal hash");
                    return;
                }
                else
                {
                    nlohmann::json who;
                    who["command"] = "get_list";
                    SPDLOG_TRACE("Get list");
                    webSocket.sendBinary(nlohmann::json::to_cbor(who));
                   
                }
               
            }
            if (pack["command"] == "send_list"&& pack.count("file_list"))
            {
                auto file_list = pack["file_list"].get<std::map<std::string, std::size_t>>();
                nlohmann::json data_list;
                for (auto& pair : file_hash)
                {
                    const auto& file = pair.first;
                    const auto& tuple = pair.second;
                    const auto& hash = std::get<0>(tuple);
                    const auto& content = std::get<1>(tuple);
                    if (!std::any_of(file_list.begin(), file_list.end(), [&hash, &file](const std::pair<std::string, std::size_t>& p)
                        {
                            return hash == p.second && file == p.first;
                        }))
                    {
                        SPDLOG_INFO("Send update file: {}", file);
                        data_list["data_file"][file]=nlohmann::json::binary(content);
                    }

                }
                data_list["command"] = "send_data_file";
                webSocket.sendBinary(nlohmann::json::to_cbor(data_list));

            }
        }
     //   std::cout << "Received: " << msg->str << std::endl;

      //  webSocket.send(msg->str, msg->binary);
    }
    else if (msg->type == ix::WebSocketMessageType::Ping)
    {
        SPDLOG_TRACE("Ping OK");
    }
    else if (msg->type == ix::WebSocketMessageType::Pong)
    {
        SPDLOG_TRACE("Pong OK");
    }

}
ServerUpdate::ServerUpdate(const std::string& dir_path_):dir_path(dir_path_)
{
    ix::initNetSystem();
    StartUpdate();
    // Run a server on localhost at a given port.
    // Bound host name, max connections and listen backlog can also be passed in as parameters.
    int port = 8071;
    std::string host("0.0.0.0"); // If you need this server to be accessible on a different machine, use "0.0.0.0"
    server=std::make_unique<ix::WebSocketServer>(port, host);

    server->setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState> connectionState, ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg) {
        MessageCallback(connectionState, webSocket, msg);
        });
    auto res = server->listen();
    if (!res.first)
    {
        // Error handling
        return ;
    }
    // Per message deflate connection is enabled by default. It can be disabled
// which might be helpful when running on low power devices such as a Rasbery Pi
    server->disablePerMessageDeflate();

    // Run the server in the background. Server can be stoped by calling server.stop()
    server->start();

    // Block until server.stop() is called.
    server->wait();
}

ServerUpdate::~ServerUpdate()
{
}
int main()
{
   

    ServerUpdate("D:\\test2");
    spdlog::set_level(spdlog::level::trace);




 //   while (true)
 //   {
 //       cout << "Hello CMake." << endl;
 //   }
	//
	//	//wd::watch(fs::path("D:\\test"),
	//	//	[&](const fs::path& path) { need_update.store(true, std::memory_order_seq_cst); });
	//cout << "Hello CMake." << endl;
    ix::uninitNetSystem();
	return 0;
}
