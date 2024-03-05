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
#include <codecvt> // codecvt_utf8
#include <locale>  
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
class UpdateClient
{
public:
    UpdateClient(const std::string& dir_path);
    ~UpdateClient();    
    void HashFS();
    size_t OneHash();

private:
    void MessageCallback(const ix::WebSocketMessagePtr& msg);
    std::size_t hash_file(const fs::path& filepath);
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
        
        if (pack.count("command") && pack["command"] == "send_hash")
        {
            pack["hash"] = one_hash;
            pack["command"] = "get_hash";
            
            spdlog::trace("Send hash");
        }
        else if (pack.count("command") && pack["command"] == "send_list")
        {
            pack["file_list"] = file_hash;
            pack["command"] = "get_list";
            spdlog::trace("Send list");
        }
        else if (pack.count("command") && pack["command"] == "send_data_file")
        {
            for (const auto& [file_path,data] : pack["data_file"].items())
            {
                auto path_file = fs::path(from_utf8(dir_path) + L"\\" + from_utf8(file_path));
                if (!fs::exists(path_file.parent_path())) {
                    if (!fs::create_directories(path_file.parent_path())) {
                        spdlog::error("Failed to create directory: {}", dir_path);
                        return;
                    }
                }
                
                std::ofstream file(path_file, std::ios::binary);
                if (!file.is_open()) {
                    spdlog::error("Failed to open file:{} ", file_path);
                    return ; // Возврат 0 в случае ошибки
                }
                file.seekp(std::ios::beg);
                spdlog::info("Update file :{} ", path_file.u8string());
               

                if (data.is_binary())
                {
                    file.write(reinterpret_cast<const char*>(data.get_binary().data()), data.get_binary().size());
                }
                file.close();

            }
            one_hash = OneHash();
            pack.erase("data_file");
            pack["hash"] = one_hash;
            pack["command"] = "get_hash";
            spdlog::trace("Send hash");

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
        spdlog::error(ss.str());
        
    }
    if (msg->type == ix::WebSocketMessageType::Open)
    {
        spdlog::trace("Open connection");
        nlohmann::json pack;
        pack["hash"] = one_hash;
        pack["command"] = "get_hash";
      //  std::vector<std::uint8_t> v_cbor = nlohmann::json::to_cbor(pack);
        webSocket.sendBinary(nlohmann::json::to_cbor(pack));
    }
    if (msg->type == ix::WebSocketMessageType::Close)
    {
        spdlog::trace("Close connection");
    }
    if (msg->type == ix::WebSocketMessageType::Ping)
    {
        spdlog::trace("Ping OK");
    }
    if (msg->type == ix::WebSocketMessageType::Pong)
    {
        spdlog::trace("Pong OK");
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
            file_hash.insert_or_assign(relative_path.u8string(), hash_file(entry.path()));
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
std::size_t UpdateClient::hash_file(const fs::path& filepath) {
    // Открываем файл для чтения в бинарном режиме
    std::ifstream file(filepath, std::ios::binary);
    while (!file.is_open()) {
        spdlog::error("Failed to open file:{} ", filepath.u8string());
        std::this_thread::sleep_for(5s);
     //   return 0; // Возврат 0 в случае ошибки
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
