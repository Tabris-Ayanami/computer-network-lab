// 终端界面：显示数据、读取键盘、收集表单。聊天业务由 chat.cpp 提供。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include "chat.h"
using namespace std;

ChatView view;                      // 核心提供的显示副本；修改它不会改动核心数据。
wstring draft;                      // 尚未发送的聊天草稿，使用 Windows 宽字符。
int scroll_lines = 0;
enum Pane { GroupPane, ChatPane, SystemPane };
enum Page { Menu, NewGroup, AddMember, Members };
int focus = ChatPane;               // 当前栏：0 左、1 中、2 右。
Page page = Menu;                   // 右栏当前页面。
int choice = 0;                     // 右栏选项，从 0 开始。
uint32_t pointed_group = 0;          // 左栏指向的群，Enter 才请求核心切群。
uint32_t opened_group = 0;           // 仅用于判断画面是否需要重置滚动位置。
vector<wstring> form;               // 建群 1 个字段，加人 3 个字段。
HANDLE screen = GetStdHandle(STD_OUTPUT_HANDLE);
COORD origin{};
WORD normal_color = 7;

// 将 Windows 的 UTF-16 字符串转换成 UTF-8，返回普通 string。
// 参数通常来自键盘或命令行；网络、data 文件和业务数据统一使用 UTF-8。
// 第一次调用计算所需字节数，第二次才把转换结果写进 result。
string utf8(const wstring& text) {
    int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), count, nullptr, nullptr);
    return result;
}

// 与 utf8 相反，把网络/文件里的 UTF-8 转成 WriteConsoleW 能显示的宽字符串。
// 返回 wstring；本函数只转换文字，不修改群表，也不进行网络通信。
wstring wide(const string& text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), count);
    return result;
}

// 启动时显示问题并读取一整行，返回 UTF-8，支持中文昵称。
// 这里只在正式绘制 TUI 之前调用；去掉行尾的回车、换行，保留名字里的空格。
string ask(const wstring& label) {
    DWORD count;
    WriteConsoleW(screen, label.data(), (DWORD)label.size(), &count, nullptr);
    wchar_t buffer[256];
    if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), buffer, 256, &count, nullptr)) {
        return {};
    }
    wstring text(buffer, count);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return utf8(text);
}

// 显示登录/注册菜单，收集输入并调用核心接口；返回 false 表示退出。
// UI 只把数字输入转成 int，账号文件读取和存盘由 chat.cpp 完成。
bool choose_account(int argc, wchar_t* argv[]) {
    auto accounts = chat_accounts();
    if (argc > 1) {
        if (argc == 2) {
            auto item = accounts.find(_wtoi(argv[1]));
            if (item != accounts.end() && chat_account(item->second, false)) {
                return true;
            }
        }
        ask(L"只能使用 chat.exe 已注册端口 登录；新账号请双击后注册。按回车退出。");
        return false;
    }
    while (true) {
        string option = ask(L"\n1. 登录已有账号\n2. 注册新账号\n0. 退出\n请选择：");
        if (option == "0") {
            return false;
        }
        if (option == "1") {
            if (accounts.empty()) {
                ask(L"还没有账号，请先注册。按回车返回。");
                continue;
            }
            wstring list = L"\n已有账号：\n";
            int index = 0;
            for (const auto& item : accounts) {
                const Member& account = item.second;
                list += to_wstring(++index) + L". " + wide(account.name) + L"  " + wide(account.ip)
                     + L":" + to_wstring(account.port) + L"\n";
            }
            int number = atoi(ask(list + L"输入账号序号：").c_str());
            if (number >= 1 && number <= (int)accounts.size()) {
                if (chat_account(next(accounts.begin(), number - 1)->second, false)) {
                    return true;
                }
            }
        } else if (option == "2") {
            Member account;
            account.name = ask(L"新账号昵称（注册后固定）：");
            account.port = atoi(ask(L"监听端口（例如 9001）：").c_str());
            account.ip = ask(L"本机 IPv4（同机测试直接回车）：");
            if (chat_account(account, true)) {
                return true;
            }
            ask(wide(chat_snapshot().status) + L"按回车返回。");
        }
    }
}

// 返回一个字符在终端中占几格：本实验按常见中文两格、英文一格处理。
// 绘制、换行、输入框裁剪共用这条规则；复杂 emoji 的宽度没有专门处理。
int cell_width(wchar_t ch) {
    return ch >= 0x2e80 ? 2 : 1;
}

// 在界面相对坐标 x、y 写一段文字，超过 width 格就截断；返回实际占用格数。
// highlight=true 用蓝底表示当前选项。origin 把界面坐标换成控制台缓冲区坐标。
// WriteConsoleW 负责显示中文，SetConsoleCursorPosition 负责定位，不打印换行。
int put(int x, int y, const wstring& text, int width, bool highlight = false) {
    wstring clipped;
    int cells = 0;
    for (wchar_t ch : text) {
        int size = cell_width(ch);
        if (cells + size > width) {
            break;
        }
        clipped += ch;
        cells += size;
    }
    if (highlight) {
        clipped += wstring(width - cells, L' ');
    }
    SetConsoleTextAttribute(screen, highlight ? BACKGROUND_BLUE | 15 : normal_color);
    SetConsoleCursorPosition(screen, {(SHORT)(origin.X + x), (SHORT)(origin.Y + y)});
    DWORD written;
    WriteConsoleW(screen, clipped.data(), (DWORD)clipped.size(), &written, nullptr);
    SetConsoleTextAttribute(screen, normal_color);
    return cells;
}

// 把一段文字按 width 格切成多行，供中间聊天区显示完整的长消息。
// result 由调用者提供，函数把切好的行追加进去；它不会更改原始聊天记录。
void wrap_lines(const wstring& text, int width, vector<wstring>& result) {
    wstring line;
    int cells = 0;
    for (wchar_t ch : text) {
        int size = cell_width(ch);
        if (cells + size > width) {
            result.push_back(line);
            line.clear();
            cells = 0;
        }
        line += ch;
        cells += size;
    }
    result.push_back(line);
}

// 返回 text 的末尾 width 格，聊天输入和右侧表单都使用这个裁剪方法。
// 原文不修改，因此画面显示不全不等于丢掉了输入的内容。
wstring input_tail(const wstring& text, int width) {
    int cells = 0;
    for (wchar_t ch : text) {
        cells += cell_width(ch);
    }
    size_t first = 0;
    while (cells > width && first < text.size()) {
        cells -= cell_width(text[first]);
        ++first;
    }
    return text.substr(first);
}

// 返回右栏当前页面的选项；绘制和上下键都使用这份列表，选项数不用另算。
// 表单的字段也是选项，后面接确认/取消；成员页按 Enter 可返回菜单。
vector<wstring> system_items() {
    if (page == NewGroup) {
        return {L"群名：", L"[ 确认 ]", L"[ 取消 ]"};
    }
    if (page == AddMember) {
        return {L"IP 地址：", L"监听端口：", L"昵称：", L"[ 确认 ]", L"[ 取消 ]"};
    }
    if (page == Members) {
        vector<wstring> items{L"[ 返回菜单 ]"};
        if (view.groups.count(view.selected)) {
            for (const auto& item : view.groups.at(view.selected).members) {
                items.push_back(wide(item.second.name));
            }
        }
        return items;
    }
    return {L"[ 创建群组 ]", L"[ 添加成员 ]", L"[ 查看成员 ]", L"[ 退出程序 ]"};
}

// 读取当前窗口尺寸并重画三栏；坐标只在本函数内使用，不再做鼠标命中判断。
// 蓝底表示键盘当前指向的选项，左侧 * 表示中间实际打开的群。
// 只在 UI 主线程中调用，使用主循环取得的数据副本绘制。
void draw() {
    if (view.selected != opened_group) {
        opened_group = view.selected;
        pointed_group = view.selected;
        scroll_lines = 0;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(screen, &info);
    origin = {info.srWindow.Left, info.srWindow.Top};
    int width = info.srWindow.Right - origin.X;
    int height = info.srWindow.Bottom - origin.Y;
    for (int y = 0; y < height; ++y) {
        put(0, y, wstring(width, L' '), width);
    }
    if (width < 79 || height < 25) {
        put(0, 0, L"请将窗口调大到至少 80 列、26 行。", width);
        return;
    }
    int left = 20;
    int panel = width - 27;
    int panel_width = 26;
    int chat_x = left + 2;
    int chat_width = panel - chat_x - 3;
    int body_rows = height - 9;
    put(0, 0, wide("Socket Chat  |  " + view.me.name + "  " + view.me.ip + ":" + to_string(view.me.port)), width);
    put(0, 1, wstring(width, L'='), width);
    put(0, 2, focus == GroupPane ? L"[群列表]" : L"群列表", left);
    put(panel, 2, focus == SystemPane ? L"[系统操作]" : L"系统操作", panel_width);
    for (int y = 2; y < height - 2; ++y) {
        put(left, y, L"|", 1);
        put(panel - 2, y, L"|", 1);
    }

    // pointed_group 用群 ID 保存，即使网络收到新群、map 顺序改变，也不会指错群。
    if (!view.groups.count(pointed_group)) {
        pointed_group = view.selected;
    }
    int pointed_index = 0;
    int index = 0;
    for (const auto& item : view.groups) {
        if (item.first == pointed_group) {
            pointed_index = index;
        }
        ++index;
    }
    int first_group = max(0, pointed_index - body_rows + 1);
    index = 0;
    for (const auto& item : view.groups) {
        if (index >= first_group && index < first_group + body_rows) {
            wstring label = item.first == view.selected ? L"* " : L"  ";
            label += wide(item.second.name);
            if (item.second.unread > 0) {
                label += L" (" + to_wstring(item.second.unread) + L")";
            }
            put(0, 4 + index - first_group, label, left,
                focus == GroupPane && item.first == pointed_group);
        }
        ++index;
    }

    // 中间始终显示聊天；先将消息换行，再截取当前能显示的部分。
    vector<wstring> lines;
    wstring title = focus == ChatPane ? L"[聊天] " : L"聊天 ";
    if (view.groups.count(view.selected)) {
        const Group& group = view.groups.at(view.selected);
        title += wide(group.name) + L"  " + to_wstring(group.members.size()) + L"人";
        for (const auto& item : group.messages) {
            wrap_lines(wide(item.second), chat_width, lines);
        }
    }
    put(chat_x, 2, title, chat_width);
    if (lines.empty()) {
        lines.push_back(L"暂无消息，在下方输入文字聊天。");
    }
    scroll_lines = min(scroll_lines, max(0, (int)lines.size() - body_rows));
    int last = (int)lines.size() - scroll_lines;
    int first = max(0, last - body_rows);
    for (int i = first; i < last; ++i) {
        put(chat_x, 4 + i - first, lines[i], chat_width);
    }
    put(chat_x, height - 5, wstring(chat_width, L'-'), chat_width);
    put(chat_x, height - 4, L"消息输入  Enter 发送", chat_width);
    int cells = put(chat_x, height - 3, L"> " + input_tail(draft, chat_width - 2), chat_width);
    int cursor_x = chat_x + cells;
    int cursor_y = height - 3;

    // 右栏每项占两行：第一行是名称，第二行可显示字段值或成员地址。
    vector<wstring> items = system_items();
    choice = min(choice, (int)items.size() - 1);
    int first_item = max(0, choice - body_rows / 2 + 1);
    for (int i = first_item; i < min((int)items.size(), first_item + body_rows / 2); ++i) {
        int row = 4 + (i - first_item) * 2;
        bool active = focus == SystemPane && choice == i;
        put(panel, row, items[i], panel_width, active);
        cells = 0;
        if ((page == NewGroup || page == AddMember) && i < (int)form.size()) {
            cells = put(panel, row + 1, L"> " + input_tail(form[i], panel_width - 2), panel_width);
            if (active) {
                ++row;             // 正在编辑字段时，光标放在第二行文字末尾。
            }
        } else if (page == Members && i > 0) {
            const auto& members = view.groups.at(view.selected).members;
            const Member& member = next(members.begin(), i - 1)->second;
            put(panel, row + 1, wide(member.ip + ":" + to_string(member.port)), panel_width);
        }
        if (active) {
            cursor_x = panel + cells;
            cursor_y = row;
        }
    }
    if (focus == GroupPane) {
        cursor_x = 0;
        cursor_y = 4 + pointed_index - first_group;
    }
    put(0, height - 2, wide(view.status), width);
    put(0, height - 1, L"左右：切换栏  上下：选择/翻页  Enter：确认/发送  表单末尾可取消", width);
    SetConsoleCursorPosition(screen, {(SHORT)(origin.X + cursor_x), (SHORT)(origin.Y + cursor_y)});
}

// 处理 Enter：调用核心接口打开群、发送消息，或提交右栏表单。
// 返回 false 表示退出。这里只切换页面、收集字段，不生成协议或修改群资料。
bool confirm() {
    if (focus == GroupPane) {
        if (chat_action(ChatAction::Select, pointed_group)) {
            scroll_lines = 0;
            focus = ChatPane;
        }
        return true;
    }
    if (focus == ChatPane) {
        if (chat_action(ChatAction::Send, view.selected, utf8(draft))) {
            draft.clear();
            scroll_lines = 0;
        }
        return true;
    }
    if (page == Menu) {
        if (choice == 3) {
            return false;
        }
        if (choice == 0) {
            page = NewGroup;
            form.assign(1, L"");
        } else if (choice == 1) {
            page = AddMember;
            form.assign(3, L"");
        } else {
            page = Members;
        }
        choice = 0;
        return true;
    }
    if (page == Members) {
        page = Menu;
        choice = 2;
        return true;
    }
    if (choice < (int)form.size()) {
        ++choice;                  // 字段里 Enter 到下一项，最后一项之后是“确认”。
        return true;
    }
    if (choice == (int)form.size()) {
        bool success;
        if (page == NewGroup) {
            success = chat_action(ChatAction::Create, 0, utf8(form[0]));
        } else {
            Member member{utf8(form[0]), _wtoi(form[1].c_str()), utf8(form[2])};
            success = chat_action(ChatAction::Add, view.selected, "", member);
        }
        if (!success) {
            return true;           // 核心留下错误提示，表单保留，方便修改。
        }
    }
    choice = page == NewGroup ? 0 : 1;
    page = Menu;
    form.clear();                  // 选择“取消”也走这里，聊天草稿 draft 保留。
    return true;
}

// 处理一次按下事件，返回是否需要重画；quit 由 Enter 选中“退出程序”时设为 true。
// 方向键只移动焦点/选项；普通 Unicode 字符只写入聊天框或当前表单字段。
// 输入法转换键没有提交文字时直接返回 false，避免无意义重画干扰中文选词。
bool key_input(const KEY_EVENT_RECORD& key, bool& quit) {
    int code = key.wVirtualKeyCode;
    if (code == VK_LEFT || code == VK_RIGHT) {
        focus = clamp(focus + (code == VK_RIGHT ? 1 : -1), 0, 2);
        return true;
    }
    if (code == VK_UP || code == VK_DOWN) {
        int step = code == VK_UP ? -1 : 1;
        if (focus == GroupPane && !view.groups.empty()) {
            auto item = view.groups.find(pointed_group);
            if (item == view.groups.end()) {
                item = view.groups.begin();
            }
            if (step < 0 && item != view.groups.begin()) {
                --item;
            } else if (step > 0 && next(item) != view.groups.end()) {
                ++item;
            }
            pointed_group = item->first;
        } else if (focus == ChatPane) {
            scroll_lines = max(0, scroll_lines - step * 3);
        } else if (focus == SystemPane) {
            choice = clamp(choice + step, 0, (int)system_items().size() - 1);
        }
        return true;
    }
    wchar_t ch = key.uChar.UnicodeChar;
    if (ch == L'\r') {
        quit = !confirm();
        return true;
    }
    wstring* target = nullptr;      // 只让可编辑的位置接收文字，菜单不会吞入聊天草稿。
    if (focus == ChatPane) {
        target = &draft;
    } else if (focus == SystemPane && (page == NewGroup || page == AddMember)
               && choice < (int)form.size()) {
        target = &form[choice];
    }
    if (target && ch == L'\b') {
        if (!target->empty()) {
            target->pop_back();
        }
        return true;
    }
    if (target && ch >= 32) {
        target->append(key.wRepeatCount, ch);
        return true;
    }
    return false;
}

// 程序入口：显示账号菜单，启动核心，然后只在本线程读取输入和绘制界面。
// 每次最多等键盘 100ms，再检查核心的更新标记；没有变化就不重画。
// ReadConsoleInputW 保留原生中文输入方式，避免 _getwch 临时清零输入模式。
int wmain(int argc, wchar_t* argv[]) {
    HANDLE keyboard = GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    GetConsoleMode(keyboard, &old_mode);
    SetConsoleMode(keyboard, (old_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)
                            & ~ENABLE_VIRTUAL_TERMINAL_INPUT);
    if (!choose_account(argc, argv)) {
        SetConsoleMode(keyboard, old_mode);
        return 0;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(screen, &info);
    normal_color = info.wAttributes;
    if (!chat_start()) {
        ask(wide(chat_snapshot().status) + L"按回车退出。");
        SetConsoleMode(keyboard, old_mode);
        return 1;
    }
    // LINE/ECHO/PROCESSED 标志保留给控制台输入法；由本程序读取事件并自行绘制。
    DWORD mode = old_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
               | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
    mode &= ~(ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(keyboard, mode);
    view = chat_snapshot();
    draw();
    bool quit = false;
    while (!quit) {
        bool redraw = false;
        DWORD ready = WaitForSingleObject(keyboard, 100);
        if (ready == WAIT_FAILED) {
            break;
        }
        if (ready == WAIT_OBJECT_0) {
            INPUT_RECORD event{};
            DWORD count = 0;
            if (!ReadConsoleInputW(keyboard, &event, 1, &count) || count == 0) {
                break;
            }
            if (event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown) {
                redraw = key_input(event.Event.KeyEvent, quit);
            } else if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                redraw = true;
            }
        }
        view = chat_snapshot();
        if (view.changed) {
            redraw = true;
        }
        if (redraw && !quit) {
            draw();
        }
    }
    chat_stop();
    SetConsoleMode(keyboard, old_mode);
    system("cls");
    return 0;
}
