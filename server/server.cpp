// updater.cpp: определяет точку входа для приложения.
//

#include "server.h"

#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <filesystem>
#include <functional>
#include <fstream>
#include "json.hpp"
#include "spdlog/spdlog.h"
#include <sstream>
#include <efsw/efsw.hpp>
#include <codecvt> // codecvt_utf8
#include <locale>  // wstring_convert

using namespace std;
namespace fs = std::filesystem;
std::wstring from_utf8(const std::string& utf8_string) {
    static std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8_conv;
    return utf8_conv.from_bytes(utf8_string);
}
std::string to_utf8(const std::wstring& wide_string)
{
    static std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8_conv;
    return utf8_conv.to_bytes(wide_string);
}
class UpdateListener : public efsw::FileWatchListener {
public:
    UpdateListener(std::unordered_map<std::string, std::atomic_bool>& need_update_):need_update(need_update_)
    {

    }
    void handleFileAction(efsw::WatchID watchid, const std::string& dir,
        const std::string& filename, efsw::Action action,
        std::string oldFilename) override {
        switch (action) {
        case efsw::Actions::Add:
            for (auto& k : need_update)
            {
                k.second.store(true);
            }
            spdlog::info("File add:{}", filename);
            break;
        case efsw::Actions::Delete:
            break;
        case efsw::Actions::Modified:
            for (auto& k : need_update)
            {
                k.second.store(true);
            }
            spdlog::info("File modified:{}", filename);
            break;
        case efsw::Actions::Moved:
            break;
        default:
            std::cout << "Should never happen!" << std::endl;
        }
    }
private:
    std::unordered_map<std::string, std::atomic_bool>&                                            need_update;
};
class ServerUpdate
{
public:
    ServerUpdate(const std::string& dir_path_);
    ~ServerUpdate();
    void MessageCallback(std::shared_ptr<ix::ConnectionState> connectionState, ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg);
    void StartUpdate();
private:
    void HashFS();
   // size_t OneHash();
    std::tuple<std::size_t, std::vector<uint8_t>> ServerUpdate::hash_file(const fs::path& filepath);
  //  std::size_t one_hash{ 0 };
    std::unique_ptr< ix::WebSocketServer> server;
    std::unordered_map<std::string,std::atomic_bool>                                            need_update;
    std::string dir_path;
    std::map<std::string, std::tuple<std::size_t,std::vector<uint8_t>>> file_hash;
    efsw::FileWatcher* fileWatcher;
    UpdateListener* listener;
};
void ServerUpdate::StartUpdate()
{
    
    HashFS();
    efsw::WatchID watchID = fileWatcher->addWatch(dir_path, listener, true);
    fileWatcher->watch();
   
    
}
void ServerUpdate::HashFS()
{

    for (const auto& entry : fs::recursive_directory_iterator(dir_path))
        if (entry.is_regular_file())
        {
            fs::path relative_path = entry.path().lexically_relative(dir_path);
            file_hash.insert_or_assign(relative_path.u8string(), hash_file(entry.path()));


        }
}

std::tuple<std::size_t, std::vector<uint8_t>> ServerUpdate::hash_file(const fs::path& filepath) {
    // Открываем файл для чтения в бинарном режиме
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("Failed to open file:{} ", filepath.u8string());
        std::this_thread::sleep_for(5s);
        HashFS();
       // return std::make_tuple(0,std::vector<uint8_t>()); // Возврат 0 в случае ошибки
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
    if (need_update.count(connectionState->getId())==0)
    {
        need_update[connectionState->getId()].store(false);
    }
    if (need_update[connectionState->getId()].load() == true)
    {
        HashFS();
        need_update[connectionState->getId()].store(false);
        nlohmann::json pack;
        pack["command"] = "send_hash";
        webSocket.sendBinary(nlohmann::json::to_cbor(pack));
        spdlog::trace("Need update");
        spdlog::trace("Send hash");
        return;

    }
    if (msg->type == ix::WebSocketMessageType::Open)
    {
        std::string header;
        for (auto it : msg->openInfo.headers)
        {
            header += it.first + ": " + it.second + " ";
        }
        spdlog::info("Remote ip:{} id:{} Uri:{} Headers:{} New connection", connectionState->getRemoteIp(), connectionState->getId(), msg->openInfo.uri, header);
        nlohmann::json pack;
        pack["command"] = "send_hash";
        webSocket.sendBinary(nlohmann::json::to_cbor(pack));
        spdlog::trace("Send hash");
    }
    else if (msg->type == ix::WebSocketMessageType::Message)
    {
        // For an echo server, we just send back to the client whatever was received by the server
        // All connected clients are available in an std::set. See the broadcast cpp example.
        // Second parameter tells whether we are sending the message in binary or text mode.
        // Here we send it in the same mode as it was received.

        if (!msg->str.empty())
        {
            nlohmann::json pack=nlohmann::json::from_cbor(msg->str);
            if (pack["command"]=="get_hash" && pack.count("hash"))
            {
                auto files_client = pack["hash"].get<std::unordered_map<std::string, std::size_t>>();
                std::unordered_map<std::string, std::tuple<std::size_t, std::vector<uint8_t>>> file_list;
                std::copy_if(file_hash.begin(), file_hash.end(), std::inserter(file_list, file_list.end()), [&files_client](const auto& p) {
                    auto search = files_client.find(p.first);
                    if (auto search = files_client.find(p.first); search != files_client.end())
                        return search->second != std::get<0>(p.second);
                    else
                        return true;

                    });
                if (file_list.empty())
                {
                    spdlog::trace("Remote ip:{} Equal hash", connectionState->getRemoteIp());
                    return;
                }
                else
                {
                    nlohmann::json data_list;
                    for (auto& pair : file_list)
                    {
                        const auto& file = pair.first;
                        const auto& content = std::get<1>(pair.second);
                        spdlog::trace("Remote ip:{} Send update file : {}", connectionState->getRemoteIp(), file);
                        data_list["data_file"][file] = nlohmann::json::binary(content);

                    }
                    data_list["command"] = "send_data_file";
                    spdlog::trace("Remote ip:{} Send data file", connectionState->getRemoteIp());
                    webSocket.sendBinary(nlohmann::json::to_cbor(data_list));
                }
               
            }
          
        }
    }
    else if (msg->type == ix::WebSocketMessageType::Ping)
    {
        spdlog::trace("Remote ip:{} Ping OK", connectionState->getRemoteIp());
    }
    else if (msg->type == ix::WebSocketMessageType::Pong)
    {
        spdlog::trace("Remote ip:{} Pong OK", connectionState->getRemoteIp());
    }
    else if (msg->type == ix::WebSocketMessageType::Close)
    {
        need_update.erase(connectionState->getId());
        spdlog::trace("Remote ip:{} Close connection", connectionState->getRemoteIp());
    }


}
ServerUpdate::ServerUpdate(const std::string& dir_path_):dir_path(dir_path_)
{
    ix::initNetSystem();
    fileWatcher = new efsw::FileWatcher();
    listener=new UpdateListener(need_update);
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
    server->stop();
    ix::uninitNetSystem();
}
int main(int argc, char* argv[])
{
    std::string path = "D:\\test";
    if (argc > 1)
        path = argv[1];
    else
    {
        spdlog::error("Set directory");
        return 0;
    }
    spdlog::set_level(spdlog::level::trace);
   // spdlog::set_level(spdlog::level::trace);
    ServerUpdate server(path);

    
	return 0;
}
