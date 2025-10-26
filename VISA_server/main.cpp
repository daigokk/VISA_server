#pragma comment(lib, "./VISA/visa64.lib")
#include <visa.h>

#include <iostream>
#include <boost/asio.hpp>
#include <visa.h>

using boost::asio::ip::tcp;

const int PORT = 12345;
const char* VISA_RESOURCE = "USB0::0x1234::0x5678::INSTR"; // ���ۂ�VISA���\�[�X���ɒu�������Ă�������

void vi_getIdn(const ViSession resourceManager, const ViChar* instrDesc, char* ret) {
    ViSession instrument;
    ViStatus status;
    status = viOpen(resourceManager, instrDesc, VI_NULL, VI_NULL, &instrument);
    if (status < VI_SUCCESS) {
        std::cerr << "�v����̃I�[�v���Ɏ��s���܂����B" << std::endl;
        return;
    }
    status = viQueryf(instrument, "%s", "%255t", "*IDN?\n", ret);
    if (status < VI_SUCCESS) {
        std::cerr << "�v����̖₢���킹�Ɏ��s���܂����B" << std::endl;
        return;
    }
    status = viClose(instrument);
    if (status < VI_SUCCESS) {
        std::cerr << "�v����̃N���[�Y�Ɏ��s���܂����B" << std::endl;
        return;
    }
}

void vi_FindRsrc(const ViSession resourceManager) {
    // �ڑ�����Ă���v����������i��: GPIB, USB, TCPIP�Ȃǁj
    ViStatus status;
    ViFindList findList;
    ViUInt32 numInstrs;
    ViChar instrDesc[256], ret[256];
    status = viFindRsrc(resourceManager, "?*INSTR", &findList, &numInstrs, instrDesc);
    if (status < VI_SUCCESS) {
        std::cerr << "�v����̌����Ɏ��s���܂����B" << std::endl;
        return;
    }
    std::cout << "���������v����̐�: " << numInstrs << std::endl;
    if (numInstrs == 0) {
        std::cout << "�v���킪������܂���ł����B" << std::endl;
        return;
    }
    // �ŏ��̌v������擾
    vi_getIdn(resourceManager, instrDesc, ret);
    std::cout << "1: " << instrDesc << ", " << ret << std::endl;

    // �c��̌v������擾
    for (ViUInt32 i = 1; i < numInstrs; ++i) {
        status = viFindNext(findList, instrDesc);
        if (status < VI_SUCCESS) break;
        vi_getIdn(resourceManager, instrDesc, ret);
        std::cout << i + 1 << ": " << instrDesc << ", " << ret << std::endl;
    }

    viClose(findList);
}

void handle_client(tcp::socket& socket, ViSession instr) {
    boost::asio::streambuf buf;
    boost::asio::read_until(socket, buf, "\n");
    std::istream is(&buf);
    std::string command;
    std::getline(is, command);

    // SCPI�R�}���h���M
    ViUInt32 writeCount;
    viWrite(instr, (ViBuf)command.c_str(), command.length(), &writeCount);

    std::string reply;

    // ������ '?' �Ȃ牞����ǂݎ��
    if (!command.empty() && command.back() == '?') {
        char response[512] = { 0 };
        ViUInt32 retCount;
        ViStatus status = viRead(instr, (ViBuf)response, sizeof(response), &retCount);
        if (status >= VI_SUCCESS) {
            reply = std::string(response, retCount);
        }
        else {
            reply = "Error reading response";
        }
    }
    else {
        reply = "Command sent";
    }

    boost::asio::write(socket, boost::asio::buffer(reply + "\n"));
}


int main() {
    ViSession defaultRM, instr;
    ViStatus status = viOpenDefaultRM(&defaultRM);
    if (status < VI_SUCCESS) {
        std::cerr << "VISA Resource Manager open failed" << std::endl;
        return 1;
    }

    status = viOpen(defaultRM, (ViRsrc)VISA_RESOURCE, VI_NULL, VI_NULL, &instr);
    if (status < VI_SUCCESS) {
        std::cerr << "Failed to open VISA device" << std::endl;
        return 1;
    }

    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), PORT));
    std::cout << "VISA USBTMC Over IP server running on port " << PORT << std::endl;

    while (true) {
        tcp::socket socket(io);
        acceptor.accept(socket);
        handle_client(socket, instr);
    }

    viClose(instr);
    viClose(defaultRM);
    return 0;
}
