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
    UpdateListener(std::atomic_bool& need_update_):need_update(need_update_)
    {

    }
    void handleFileAction(efsw::WatchID watchid, const std::string& dir,
        const std::string& filename, efsw::Action action,
        std::string oldFilename) override {
        switch (action) {
        case efsw::Actions::Add:
            need_update.store(true);
            spdlog::info("File add:{}", filename);
            break;
        case efsw::Actions::Delete:
            break;
        case efsw::Actions::Modified:
            need_update.store(true);
            spdlog::info("File modified:{}", filename);
            break;
        case efsw::Actions::Moved:
            break;
        default:
            std::cout << "Should never happen!" << std::endl;
        }
    }
private:
    std::atomic_bool&                                            need_update;
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
    size_t OneHash();
    std::tuple<std::size_t, std::vector<uint8_t>> ServerUpdate::hash_file(const fs::path& filepath);
    std::size_t one_hash{ 0 };
    std::unique_ptr< ix::WebSocketServer> server;
    std::atomic_bool                                            need_update{ false };
    std::string dir_path;
    std::map<std::string, std::tuple<std::size_t,std::vector<uint8_t>>> file_hash;
    efsw::FileWatcher* fileWatcher;
    UpdateListener* listener;
};
void ServerUpdate::StartUpdate()
{
    
    one_hash = OneHash();
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
std::tuple<std::size_t, std::vector<uint8_t>> ServerUpdate::hash_file(const fs::path& filepath) {
    // Открываем файл для чтения в бинарном режиме
    std::ifstream file(filepath, std::ios::binary);
    while (!file.is_open()) {
        spdlog::error("Failed to open file:{} ", filepath.u8string());
        std::this_thread::sleep_for(5s);
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
   
    
    if (need_update.load() == true)
    {
        one_hash = OneHash();
        need_update.store(false);
        nlohmann::json pack;
        pack["command"] = "send_hash";
        webSocket.sendBinary(nlohmann::json::to_cbor(pack));
        spdlog::trace("Need update");
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
                size_t curent_hash = pack["hash"].get<size_t>();
                spdlog::trace("Hash client:{} server:{}", curent_hash, one_hash);
                if (curent_hash == one_hash)
                {
                    spdlog::trace("Remote ip:{} Equal hash", connectionState->getRemoteIp());
                    return;
                }
                else
                {
                    nlohmann::json who;
                    who["command"] = "send_list";
                    spdlog::trace("Remote ip:{} Send list", connectionState->getRemoteIp());
                    webSocket.sendBinary(nlohmann::json::to_cbor(who));
                   
                }
               
            }
            else if (pack["command"] == "get_list"&& pack.count("file_list"))
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
                        spdlog::trace("Remote ip:{} Send update file : {}", connectionState->getRemoteIp(), file);
                        data_list["data_file"][file]=nlohmann::json::binary(content);
                    }

                }
                data_list["command"] = "send_data_file";
                webSocket.sendBinary(nlohmann::json::to_cbor(data_list));

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
}
int main()
{
    
    spdlog::set_level(spdlog::level::trace);
   // spdlog::set_level(spdlog::level::trace);
    ServerUpdate("D:\\test2");
    




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
