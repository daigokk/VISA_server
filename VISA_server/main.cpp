#pragma comment(lib, "./VISA/visa64.lib")
#include <visa.h>

#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <stdexcept> 
#include <algorithm> 
#include <cctype>    
#include <locale.h> // setlocale

#include <boost/asio.hpp>

// グローバルな定数 (ポート番号)
constexpr int PORT = 12345;

/**
 * @brief 現在のマシンのプライマリIPv4アドレスを取得します。
 * @return IPv4アドレス文字列。見つからない場合やエラー時は空文字列。
 */
std::string getIPV4Address() {
    using boost::asio::ip::tcp;

    try {
        boost::asio::io_context io_context;
        std::string hostname = boost::asio::ip::host_name();
        tcp::resolver resolver(io_context);
        tcp::resolver::results_type endpoints = resolver.resolve(hostname, "");

        for (const auto& entry : endpoints) {
            auto addr = entry.endpoint().address();
            if (addr.is_v4()) {
                return addr.to_string();
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "IPv4アドレスの取得に失敗しました: " << e.what() << std::endl;
    }
    return ""; // 見つからないかエラー
}

/**
 * @brief 指定されたリソース記述子 (instrDesc) の計測器を一時的に開き、*IDN? を問い合わせます。
 * @param resourceManager VISAリソースマネージャのセッション。
 * @param instrDesc 問い合わせ対象の計測器のリソース記述子。
 * @return 計測器のIDN文字列。失敗した場合は空文字列。
 */
std::string getInstrumentIdn(ViSession resourceManager, const ViChar* instrDesc) {
    ViSession instrument;
    ViStatus status;

    status = viOpen(resourceManager, instrDesc, VI_NULL, VI_NULL, &instrument);
    if (status < VI_SUCCESS) {
        std::cerr << "getInstrumentIdn: 計測器のオープンに失敗しました (" << instrDesc << ", Status: " << status << ")" << std::endl;
        return "";
    }

    std::array<char, 256> idnBuffer = { 0 };
    status = viQueryf(instrument, "%s", "%255t", "*IDN?\n", idnBuffer.data());

    viClose(instrument);

    if (status < VI_SUCCESS) {
        std::cerr << "getInstrumentIdn: *IDN? の問い合わせに失敗しました (" << instrDesc << ", Status: " << status << ")" << std::endl;
        return "";
    }

    return std::string(idnBuffer.data());
}

// 文字列を小文字に変換するヘルパー関数
std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/**
 * @brief 接続されている計測器を検索し、IDNに指定されたキー文字列 (key) を含む最初の計測器を見つけます。(大文字小文字を区別しない)
 * @param resourceManager VISAリソースマネージャのセッション。
 * @param key IDNに含まれるべきキーワード (例: "TEKTRONIX")。
 * @return 見つかった計測器のリソース記述子文字列。見つからない場合は空文字列。
 */
std::string findInstrument(ViSession resourceManager, const std::string& key) {
    ViStatus status;
    ViFindList findList;
    ViUInt32 numInstrs = 0;

    std::array<ViChar, VI_FIND_BUFLEN> instrDesc;

    status = viFindRsrc(resourceManager, "?*INSTR", &findList, &numInstrs, instrDesc.data());
    if (status < VI_SUCCESS) {
        std::cerr << "findInstrument: 計測器の検索 (viFindRsrc) に失敗しました (Status: " << status << ")" << std::endl;
        return "";
    }
    if (numInstrs == 0) {
        std::cout << "findInstrument: 計測器が見つかりませんでした。" << std::endl;
        return "";
    }

    std::cout << "見つかった計測器の数: " << numInstrs << std::endl;

    std::string foundAddress = "";
    const std::string lower_key = toLower(key);

    for (ViUInt32 i = 0; i < numInstrs; ++i) {
        if (i > 0) {
            status = viFindNext(findList, instrDesc.data());
            if (status < VI_SUCCESS) continue;
        }

        std::string idn = getInstrumentIdn(resourceManager, instrDesc.data());

        std::cout << "  " << (i + 1) << ": " << instrDesc.data()
            << (idn.empty() ? " (IDN取得失敗)" : " (IDN: " + idn + ")") << std::endl;

        if (!idn.empty()) {
            std::string lower_idn = toLower(idn);
            if (lower_idn.find(lower_key) != std::string::npos) {
                std::cout << "==> 対象の計測器が見つかりました: " << instrDesc.data() << std::endl;
                foundAddress = std::string(instrDesc.data());
                break;
            }
        }
    }

    viClose(findList);

    if (foundAddress.empty()) {
        std::cout << "findInstrument: 対象の計測器 (" << key << ") が見つかりませんでした (大文字小文字無視)。" << std::endl;
    }

    return foundAddress;
}

/**
 * @brief TCPクライアントからの接続を処理し、受信したコマンドをVISA計測器に転送します。
 * @param socket クライアントとの通信用ソケット。
 * @param instr 通信対象のVISA計測器セッション。
 */
void handle_client(boost::asio::ip::tcp::socket& socket, ViSession instr) {
    using boost::asio::ip::tcp;
    constexpr size_t READ_BUFFER_SIZE = 2048;

    try {
        boost::asio::streambuf buf;
        boost::system::error_code read_error;
        boost::asio::read_until(socket, buf, "\n", read_error);

        if (read_error == boost::asio::error::eof) {
            std::cout << "クライアントがコマンド送信前に切断しました。" << std::endl;
            return;
        }
        else if (read_error) {
            throw boost::system::system_error(read_error);
        }

        std::istream is(&buf);
        std::string command;
        std::getline(is, command);

        command.erase(command.find_last_not_of("\r\n") + 1);
        if (command.empty()) {
            return;
        }

        std::cout << "受信: " << command << std::endl;

        std::string visa_command = command + "\n";
        ViUInt32 writeCount;
        ViStatus status = viWrite(instr, (ViBuf)visa_command.c_str(), visa_command.length(), &writeCount);

        if (status < VI_SUCCESS) {
            std::cerr << "viWrite に失敗しました (Status: " << status << ")" << std::endl;
            boost::asio::write(socket, boost::asio::buffer("エラー: 計測器への書き込みに失敗しました\n"));
            return;
        }

        std::string reply = "コマンド送信完了 (応答なし)";

        if (command.back() == '?') {
            std::vector<char> response_buffer(READ_BUFFER_SIZE, 0);
            ViUInt32 retCount = 0;

            status = viRead(instr, (ViBuf)response_buffer.data(), READ_BUFFER_SIZE - 1, &retCount);

            if (status >= VI_SUCCESS) {
                reply = std::string(response_buffer.data(), retCount);
            }
            else {
                std::cerr << "viRead に失敗しました (Status: " << status << ")" << std::endl;
                reply = "エラー: 応答の読み取りに失敗しました";
            }
        }

        std::cout << "送信: " << reply;

        boost::asio::write(socket, boost::asio::buffer(reply + "\n"));

    }
    catch (const std::exception& e) {
        std::cerr << "handle_client で例外発生: " << e.what() << std::endl;
        try {
            boost::asio::write(socket, boost::asio::buffer(std::string("サーバーエラー: ") + e.what() + "\n"));
        }
        catch (...) {
            // 通知失敗
        }
    }
}


int main() {
    // Windowsコンソールでの日本語文字化け対策
    setlocale(LC_ALL, "japanese");

    using boost::asio::ip::tcp;

    std::cout << "VISA USBTMC Over IP サーバーを起動します..." << std::endl;

    ViSession defaultRM = VI_NULL;
    ViSession instr = VI_NULL;
    ViStatus status;

    status = viOpenDefaultRM(&defaultRM);
    if (status < VI_SUCCESS) {
        std::cerr << "VISAリソースマネージャのオープンに失敗しました (Status: " << status << ")" << std::endl;
        return 1;
    }

    std::string instrAddress = findInstrument(defaultRM, "TEKTRONIX");

    if (instrAddress.empty()) {
        std::cerr << "対象の計測器 (TEKTRONIX) の検索に失敗しました。" << std::endl;
        viClose(defaultRM);
        return 1;
    }

    status = viOpen(defaultRM, (ViRsrc)instrAddress.c_str(), VI_NULL, VI_NULL, &instr);

    if (status < VI_SUCCESS) {
        std::cerr << "VISAデバイスのオープンに失敗しました: " << instrAddress << " (Status: " << status << ")" << std::endl;
        viClose(defaultRM);
        return 1;
    }

    std::cout << "計測器のオープンに成功: " << instrAddress << std::endl;

    try {
        boost::asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), PORT));

        std::string ip = getIPV4Address();
        if (ip.empty()) {
            std::cerr << "警告: ローカルIPアドレスを決定できませんでした。'YOUR_IP_ADDRESS' を使用します。" << std::endl;
            ip = "YOUR_IP_ADDRESS";
        }

        std::cout << "\n========================================================" << std::endl;
        std::cout << "サーバー待機中。以下のVISAアドレスで接続してください:" << std::endl;
        std::cout << "TCPIP0::" << ip << "::" << PORT << "::SOCKET" << std::endl;
        std::cout << "========================================================\n" << std::endl;

        while (true) {
            tcp::socket socket(io);
            acceptor.accept(socket);

            try {
                std::cout << "クライアントが接続しました: " << socket.remote_endpoint().address().to_string() << std::endl;

                handle_client(socket, instr);

                std::cout << "クライアントが切断しました。\n\n";

            }
            catch (const std::exception& e) {
                std::cerr << "クライアント接続処理中にエラーが発生しました: " << e.what() << std::endl;
            }
        }

    }
    catch (const std::exception& e) {
        std::cerr << "サーバーのセットアップに失敗、または致命的なエラーが発生しました: " << e.what() << std::endl;
    }

    std::cout << "シャットダウンしています..." << std::endl;
    if (instr != VI_NULL) {
        viClose(instr);
    }
    if (defaultRM != VI_NULL) {
        viClose(defaultRM);
    }

    return 0;
}