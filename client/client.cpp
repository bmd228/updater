// updater.cpp: определяет точку входа для приложения.
//

#include "client.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <sstream>
#include <filesystem>
#include <functional>
#include <fstream>
#include "json.hpp"
#include "spdlog/spdlog.h"
using namespace std;
namespace fs = std::filesystem;

class UpdateClient
{
public:
    UpdateClient(const std::string& dir_path);
    ~UpdateClient();    
    void HashFS();
    size_t OneHash();

private:
    void MessageCallback(const ix::WebSocketMessagePtr& msg);
    std::size_t hash_file(const std::string& filepath);
    std::size_t one_hash{0};
    const std::string dir_path;
    ix::WebSocket webSocket;
    std::map<std::string, std::size_t> file_hash;
};

UpdateClient::UpdateClient(const std::string& dir_path_):dir_path(dir_path_)
{
    one_hash = OneHash();
    ix::initNetSystem();
    std::string url("ws://localhost:8071/");
    webSocket.setUrl(url);

    // Optional heart beat, sent every 45 seconds when there is not any traffic
    // to make sure that load balancers do not kill an idle connection.
    webSocket.setPingInterval(45);

    // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
    webSocket.disablePerMessageDeflate();
    webSocket.enableAutomaticReconnection();
    // Setup a callback to be fired when a message or an event (open, close, error) is received
    webSocket.setOnMessageCallback(std::bind(&UpdateClient::MessageCallback, this, std::placeholders::_1));

    webSocket.start();
}
void UpdateClient::MessageCallback(const ix::WebSocketMessagePtr& msg)
{

    if (msg->type == ix::WebSocketMessageType::Message)
    {
        
        nlohmann::json pack= nlohmann::json::from_cbor(msg->str);
        pack["hash"] = one_hash;
        if (pack.count("command") && pack["command"] == "get_list")
        {
            pack["file_list"] = file_hash;
            pack["command"] = "send_list";
            SPDLOG_TRACE("Send list");
        }
        if (pack.count("command") && pack["command"] == "send_data_file")
        {
            for (const auto& [file_path,data] : pack["data_file"].items())
            {
                
                std::ofstream file(dir_path+"\\"+ file_path, std::ios::binary);
                if (!file.is_open()) {
                    SPDLOG_ERROR("Failed to open file:{} ", file_path);
                    return ; // Возврат 0 в случае ошибки
                }
                file.seekp(std::ios::beg);
                SPDLOG_INFO("Update file :{} ", dir_path + "\\" + file_path);
               

                if (data.is_binary())
                {
                    file.write(reinterpret_cast<const char*>(data.get_binary().data()), data.get_binary().size());
                }
                file.close();

            }
            one_hash = OneHash();
       }
        webSocket.sendBinary(nlohmann::json::to_cbor(pack));
    }
    if (msg->type == ix::WebSocketMessageType::Error)
    {
        std::stringstream ss;
        ss << "Error: " << msg->errorInfo.reason << std::endl;
        ss << "#retries: " << msg->errorInfo.retries << std::endl;
        ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
        ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
        SPDLOG_ERROR(ss.str());
        
    }
    if (msg->type == ix::WebSocketMessageType::Open)
    {
        SPDLOG_TRACE("Open connection");
        nlohmann::json pack;
        pack["hash"] = one_hash;
        pack["command"] = "get_hash";
      //  std::vector<std::uint8_t> v_cbor = nlohmann::json::to_cbor(pack);
        webSocket.sendBinary(nlohmann::json::to_cbor(pack));
    }
    if (msg->type == ix::WebSocketMessageType::Close)
    {
        SPDLOG_TRACE("Close connection");
    }
}
UpdateClient::~UpdateClient()
{
    webSocket.stop();
    ix::uninitNetSystem();
}
void UpdateClient::HashFS()
{
    for (const auto& entry : fs::recursive_directory_iterator(dir_path))
        if (entry.is_regular_file())
        {
            fs::path relative_path = entry.path().lexically_relative(dir_path);
            file_hash.emplace(relative_path.u8string(), hash_file(entry.path().u8string()));
            //auto pos = entry.path().parent_path().u8string().find(dir_path);
            //if (pos != std::u16string::npos)
            //{
            //    auto sd = entry.path().parent_path().u8string().substr(pos + dir_path.size());
            //    file_hash.emplace(sd, hash_file(entry.path().u8string()));
            //}

           
       }
}
size_t UpdateClient::OneHash()
{
    HashFS();
    std::size_t result = 0;

    // Сливаем хэши отсортированных элементов вектора
    for (auto  &&[file,hash] : file_hash) {
        // Вычисляем новый хэш как XOR текущего результата и хэша элемента
        result ^= hash;
    }
    return result;
}
std::size_t UpdateClient::hash_file(const std::string& filepath) {
    // Открываем файл для чтения в бинарном режиме
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        SPDLOG_ERROR("Failed to open file:{} ", filepath);
        return 0; // Возврат 0 в случае ошибки
    }

    // Создаем объект хэш-функции
    std::hash<std::string> hasher;

    // Читаем содержимое файла и вычисляем хэш
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::size_t file_hash = hasher(content);
    file.close();
    return file_hash;
}
int main()
{

    //webSocket.send("hello world");

    //// The message can be sent in BINARY mode (useful if you send MsgPack data for example)
    //webSocket.sendBinary("some serialized binary data");
    spdlog::set_level(spdlog::level::trace);
    UpdateClient client("D:\\test");
    
    while (true)
    {
       // std::cout << "start" << std::endl;

    }

	return 0;
}
