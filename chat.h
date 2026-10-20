// 两个 cpp 共用的数据类型和接口。这里只声明，具体逻辑全部在 chat.cpp。
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Member {
    std::string ip;
    int port = 0;                   // 账号固定的监听端口。
    std::string name;
};

struct Group {
    std::string name;
    std::string packet;             // 群资料的协议原文，由核心保存和同步。
    std::map<std::string, Member> members; // IP:端口 → 成员。
    std::multimap<std::string, std::string> messages; // 协议原文 → 显示文字，自动排序并保留重复消息。
    int unread = 0;
};

// 给界面的一份数据副本。UI 改动副本不会改变核心里的账号、群表或聊天记录。
struct ChatView {
    Member me{};
    std::map<uint32_t, Group> groups{};
    uint32_t selected = 0;
    std::string status{};
    bool changed = true;           // 本次读取后，核心会清除实际数据里的更新标记。
};

// 先列出账号以初始化 data；create=true 注册，false 按文件恢复固定身份。
std::map<int, Member> chat_accounts();
bool chat_account(Member account, bool create);

// 网络生命周期：登录/注册成功后启动；退出界面时关闭。
bool chat_start();
void chat_stop();

// 一个入口处理四种操作。text 用于群名或消息，member 仅用于加人。
enum class ChatAction { Create, Add, Select, Send };
bool chat_action(ChatAction action, uint32_t id = 0, const std::string& text = "", const Member& member = {});

// 获取受互斥锁保护的数据副本，changed 表示是否需要重画。
ChatView chat_snapshot();
