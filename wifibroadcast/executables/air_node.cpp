#include <iostream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "../src/WBTxRx.h"
#include "../src/WBStreamTx.h"
#include "../src/wifibroadcast_spdlog.h"
#include "../src/WBPacketHeader.h"
#include "../src/HelperSources/SocketHelper.hpp"

// Функція для налаштування UART під телеметрію
int setup_serial(const char* port_name) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ISIG;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    cfsetispeed(&tty, B460800);
    cfsetospeed(&tty, B460800);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return -1;
    return fd;
}

int main(int argc, char *const *argv) {
    std::string card = "wlan1";
    auto console = wifibroadcast::log::create_or_get("main");

    // 1. Ініціалізація радіо-ядра (БЕЗ милиць у вигляді raw_socket)
    std::vector<wifibroadcast::WifiCard> cards = {{card, 1}};
    WBTxRx::Options txrx_options{};
    txrx_options.tx_without_pcap = true;

    auto radiotap_holder = std::make_shared<RadiotapHeaderTxHolder>();
    std::shared_ptr<WBTxRx> txrx = std::make_shared<WBTxRx>(cards, txrx_options, radiotap_holder);
    txrx->start_receiving();

    // 2. Ініціалізація ВІДЕО-потоку (Радіо-Порт 0, FEC увімкнено)
    WBStreamTx::Options video_options{};
    video_options.radio_port = 0;
    video_options.enable_fec = true;
    video_options.default_packet_type = WB_PACKET_TYPE_VIDEO;

    // Використовуємо СТАНДАРТНИЙ WBStreamTx для повної сумісності з WBStreamRx
    auto video_tx = std::make_shared<WBStreamTx>(txrx, video_options, radiotap_holder);

    SocketHelper::UDPReceiver udp_receiver(
        SocketHelper::ADDRESS_LOCALHOST, 5600,
        [video_tx](const uint8_t *payload, const std::size_t payloadSize) {
            auto frame = std::make_shared<std::vector<uint8_t>>(payload, payload + payloadSize);
            // Цей метод коректно поріже кадр, додасть FEC і загорне у WBPacketHeader
            video_tx->try_enqueue_frame(frame, 1024, 8);
        });
    udp_receiver.runInBackground();
    console->info("Production Video Stream: UDP 5600 -> WBStreamTx(FEC) -> Radio Port 0");

    // 3. Ініціалізація ТЕЛЕМЕТРІЇ (Радіо-Порт 1, FEC вимкнено)
    WBStreamTx::Options telemetry_options{};
    telemetry_options.radio_port = 1;
    telemetry_options.enable_fec = false;
    telemetry_options.default_packet_type = WB_PACKET_TYPE_TELEMETRY;
    auto telemetry_tx = std::make_shared<WBStreamTx>(txrx, telemetry_options, radiotap_holder);

    std::thread telemetry_thread([&telemetry_tx, console]() {
        int serial_fd = setup_serial("/dev/ttyAMA0");
        if (serial_fd < 0) {
            console->error("Failed to open /dev/ttyAMA0");
            return;
        }
        console->info("Telemetry UART /dev/ttyAMA0 opened -> Radio Port 1");
        uint8_t buf[256];
        while (true) {
            int n = read(serial_fd, buf, sizeof(buf));
            if (n > 0) {
                auto packet = std::make_shared<std::vector<uint8_t>>(buf, buf + n);
                telemetry_tx->enqueue_packet_dropping(packet, 2);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });

    // 4. Головний цикл
    auto lastLog = std::chrono::steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto elapsed = std::chrono::steady_clock::now() - lastLog;
        if (elapsed > std::chrono::seconds(2)) {
            lastLog = std::chrono::steady_clock::now();
            std::cout << txrx->get_tx_stats() << std::endl;
        }
    }

    telemetry_thread.join();
    return 0;
}