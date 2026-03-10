#include <iostream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "../src/WBTxRx.h"
#include "../src/WBStreamTx.h"
#include "../src/legacy/WBStreamTxUDP.h"
#include "../src/wifibroadcast_spdlog.h"
#include "../src/WBPacketHeader.h"

// Функція для налаштування UART під телеметрію
int setup_serial(const char* port_name) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;

    // Налаштування для CRSF / Сирих даних (8N1, без flow control)
    tty.c_cflag &= ~PARENB; // Без паритету
    tty.c_cflag &= ~CSTOPB; // 1 стоп-біт
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;     // 8 біт даних
    tty.c_cflag &= ~CRTSCTS; // Без апаратного flow control
    tty.c_cflag |= CREAD | CLOCAL; // Увімкнути приймач, ігнорувати лінії модема

    tty.c_lflag &= ~ICANON; // Сирий режим (не рядковий)
    tty.c_lflag &= ~ECHO;   // Без луни
    tty.c_lflag &= ~ISIG;   // Без спецсимволів (INTR, QUIT)

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // Без програмного flow control
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL); // Без обробки спецсимволів

    tty.c_oflag &= ~OPOST; // Сирий вивід

    // Швидкість. CRSF від ArduPilot зазвичай працює на 416666 або 420000 baud.
    // Стандартний termios має B460800. Якщо потрібен кастомний baud,
    // тут зазвичай використовують termios2 (TCGETS2).
    // Поки ставимо B460800 як заглушку, за потреби адаптуємо під termios2.
    cfsetispeed(&tty, B460800);
    cfsetospeed(&tty, B460800);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return -1;
    return fd;
}

int main(int argc, char *const *argv) {
    std::string card = "wlan1";
    auto console = wifibroadcast::log::create_or_get("main");

    // 1. Ініціалізація радіо-ядра
    std::vector<wifibroadcast::WifiCard> cards = {{card, 1}};
    WBTxRx::Options txrx_options{};
    txrx_options.tx_without_pcap = true; // З ваших тестів

    auto radiotap_holder = std::make_shared<RadiotapHeaderTxHolder>();
    std::shared_ptr<WBTxRx> txrx = std::make_shared<WBTxRx>(cards, txrx_options, radiotap_holder);
    txrx->start_receiving();

    // 2. Ініціалізація ВІДЕО-потоку (Радіо-Порт 0)
    WBStreamTx::Options video_options{};
    video_options.radio_port = 0;
    video_options.enable_fec = true; // Для відео обов'язково
    video_options.default_packet_type = WB_PACKET_TYPE_VIDEO;
    auto video_tx = std::make_unique<WBStreamTxUDP>(txrx, video_options, 8, 5600);
    console->info("Video Stream initialized on UDP 5600 -> Radio Port 0");

    // 3. Ініціалізація ТЕЛЕМЕТРІЇ (Радіо-Порт 1)
    WBStreamTx::Options telemetry_options{};
    telemetry_options.radio_port = 1;
    telemetry_options.enable_fec = false; // Вимикаємо FEC для мінімальної затримки
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
                // Відправляємо пакет в ефір. n_injections = 2 означає,
                // що пакет полетить 2 рази для надійності без затримок FEC.
                telemetry_tx->enqueue_packet_dropping(packet, 2);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });

    // 4. Головний цикл з виводом статистики
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