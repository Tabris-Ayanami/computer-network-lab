#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include "chat.h"
using namespace std;
ChatView state;
filesystem::path directory;
mutex data_mutex;
atomic<bool> running{false};
SOCKET listener;
thread receiver;
Member read_member(istream& input, char end = '\t') { // 成员格式为 IP、端口、昵称；最后一个字段保留空格和中文。
    Member member;
    input >> member.ip >> member.port;
    input.get();
    getline(input, member.name, end);
    return member;
}
sockaddr_in endpoint(const string& ip, int port) { // 一处设置 IPv4 地址与网络字节序，连接和监听共用。
    sockaddr_in value{AF_INET, htons((u_short)port), {}, {}};
    inet_pton(AF_INET, ip.c_str(), &value.sin_addr);
    return value;
}
bool save() { // 只保存 SELF 和原始群表，消息始终在内存中。
    ofstream file(directory / (to_string(state.me.port) + ".txt"));
    file << "SELF\t" << state.me.ip << '\t' << state.me.port << '\t' << state.me.name << '\n';
    for (auto& item : state.groups)
        file << item.second.packet << '\n';
    return (bool)file;
}
void process_packet(const string& packet, bool persist = true) { // 协议只有 GROUP 和 CHAT；本机操作和网络接收都经过这里。
    istringstream input(packet);
    string type, stamp, ip, text;
    uint32_t id = 0;
    int port = 0;
    input >> type >> id;
    input.get();
    if (type != "GROUP" && (type != "CHAT" || !state.groups.count(id)))
        return;
    Group& group = state.groups[id];
    if (type == "GROUP") {
        group.packet = packet;
        getline(input, group.name, '\t');
        group.members.clear();
        while (input.peek() != EOF) {
            Member member = read_member(input);
            group.members[member.ip + ":" + to_string(member.port)] = member;
        }
        if (persist)
            save();
        state.status = "已收到群资料：" + group.name;
    } else {
        input >> stamp >> ip >> port;
        input.get();
        getline(input, text);
        string sender = ip + ":" + to_string(port);
        string name = group.members.count(sender) ? group.members.at(sender).name : sender;
        group.messages.emplace(packet, "[" + stamp.substr(11) + "] " + name + ": " + text);
        if (id != state.selected)
            ++group.unread;
    }
    if (!state.groups.count(state.selected))
        state.selected = id;
    state.changed = true;
}
bool send_to(const Member& member, const string& text) { // 一条 TCP 连接传一条文本；循环 send 防止只发出部分字节。
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in target = endpoint(member.ip, member.port);
    bool connected = connect(connection, (sockaddr*)&target, sizeof(target)) == 0;
    size_t sent = 0;
    while (connected && sent < text.size()) {
        int count = send(connection, text.data() + sent, (int)(text.size() - sent), 0);
        if (count <= 0)
            break;
        sent += count;
    }
    closesocket(connection);
    return connected && sent == text.size();
}
void receive_loop() { // 接收线程只在更新共享数据时加锁；recv 到 EOF 才解析完整协议。
    while (running) {
        SOCKET connection = accept(listener, nullptr, nullptr);
        if (connection == INVALID_SOCKET)
            continue;
        string packet;
        char buffer[1024];
        int count;
        while ((count = recv(connection, buffer, sizeof(buffer), 0)) > 0)
            packet.append(buffer, count);
        closesocket(connection);
        lock_guard<mutex> lock(data_mutex);
        if (running && count == 0)
            process_packet(packet);
    }
}
map<int, Member> chat_accounts() { // 初始化 exe 旁的 data，并按端口列出已有账号。
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    directory = filesystem::path(path).parent_path() / L"data";
    filesystem::create_directories(directory);
    map<int, Member> accounts;
    for (auto& entry : filesystem::directory_iterator(directory)) {
        ifstream file(entry.path());
        string type;
        file >> type;
        if (type == "SELF") {
            Member member = read_member(file, '\n');
            accounts[member.port] = member;
        }
    }
    return accounts;
}
bool chat_account(Member account, bool create) { // create=true 注册，否则登录；已有文件不能被注册覆盖
    auto path = directory / (to_string(account.port) + ".txt");
    state.status = "请用已注册端口登录，或用尚未注册的端口注册。";
    if (create == filesystem::exists(path))//分类讨论保证不会覆盖已有账号，登录时必须存在文件
        return false;
    account.ip = account.ip.empty() ? "127.0.0.1" : account.ip;
    state = ChatView{account};
    if (create)
        return save();
    ifstream file(path);
    string line;
    file >> line;
    state.me = read_member(file, '\n');
    while (getline(file, line))
        process_packet(line, false);
    return true;
}
bool chat_start() { // Winsock 初始化、bind/listen 后另开线程，UI 主线程继续读取键盘。
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in local = endpoint("0.0.0.0", state.me.port);
    if (bind(listener, (sockaddr*)&local, sizeof(local)) != 0 || listen(listener, 8) != 0) {
        state.status = "监听失败，端口可能被占用。";
        closesocket(listener);
        WSACleanup();
        return false;
    }
    running = true;
    receiver = thread(receive_loop);
    return true;
}
void chat_stop() { // 发一个本机空连接，唤醒正在等待的 accept，然后等待线程结束。
    running = false;
    send_to({"127.0.0.1", state.me.port, ""}, "");
    receiver.join();
    closesocket(listener);
    WSACleanup();
}
bool chat_action(ChatAction action, uint32_t id, const string& text, const Member& member) { // 四种操作共用加锁、协议处理和群发流程，避免分别维护四套代码。
    lock_guard<mutex> lock(data_mutex);
    state.changed = true;
    ostringstream packet;
    time_t seconds = time(nullptr);
    tm local{};
    localtime_s(&local, &seconds);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H:%M:%S", &local);
    if (action == ChatAction::Create) {
        id = (uint32_t)hash<string>{}(string(stamp) + "|" + state.me.ip + ":" + to_string(state.me.port));
        packet << "GROUP\t" << id << '\t' << text << '\t' << state.me.ip << '\t' << state.me.port << '\t' << state.me.name;
    } else if (!state.groups.count(id)) {
        state.status = "请先创建或选择一个群。";
        return false;
    } else if (action == ChatAction::Add) {
        packet << state.groups.at(id).packet << '\t' << member.ip << '\t' << member.port << '\t' << member.name;
    } else if (action == ChatAction::Select) {
        state.selected = id;
        state.groups.at(id).unread = 0;
        return true;
    } else {
        if (text.empty())
            return true;
        packet << "CHAT\t" << id << '\t' << stamp << '\t' << state.me.ip << '\t' << state.me.port << '\t' << text;
    }
    process_packet(packet.str());
    if (action == ChatAction::Create)
        state.selected = id;
    state.status = action == ChatAction::Create ? "已建群：" + text : "消息或群资料已发送。";
    for (auto& item : state.groups.at(id).members) {
        const Member& target = item.second;
        if ((target.ip != state.me.ip || target.port != state.me.port) && !send_to(target, packet.str()))
            state.status = "发送失败：" + target.name;
    }
    return true;
}
ChatView chat_snapshot() { // 副本交给 UI，核心的实际数据仍由互斥锁保护。
    lock_guard<mutex> lock(data_mutex);
    ChatView view = state;
    state.changed = false;
    return view;
}
