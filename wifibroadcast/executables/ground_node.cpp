#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/WBTxRx.h"
#include "../src/WBStreamRx.h"
#include "../src/wifibroadcast_spdlog.h"

int create_udp_socket(const char* ip, int port, struct sockaddr_in& addr) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return sock;
}

int main(int argc, char *const *argv) {
    // IP адреса вашого комп'ютера з GCS.
    // Можна передати першим аргументом при запуску, інакше шлемо на локалхост
    const char* target_ip = (argc > 1) ? argv[1] : "192.168.50.10";
    std::string card = "wlan1";
    auto console = wifibroadcast::log::create_or_get("main");

    console->info("Starting Ground Node. Forwarding traffic to GCS IP: {}", target_ip);

    // Налаштування UDP сокетів для пересилки
    struct sockaddr_in video_addr, telem_addr;
    int video_sock = create_udp_socket(target_ip, 10900, video_addr);
    int telem_sock = create_udp_socket(target_ip, 2223, telem_addr);

    // 1. Ініціалізація радіо-ядра (Ground mode)
    std::vector<wifibroadcast::WifiCard> cards = {{card, 1}};
    WBTxRx::Options txrx_options{};
    txrx_options.use_gnd_identifier = true; // Обов'язково для землі
    txrx_options.tx_without_pcap = false;

    auto radiotap_holder = std::make_shared<RadiotapHeaderTxHolder>();
    std::shared_ptr<WBTxRx> txrx = std::make_shared<WBTxRx>(cards, txrx_options, radiotap_holder);
    txrx->start_receiving(); // Запуск фонового потоку слухання ефіру

    // 2. Демаршрутизація ВІДЕО (Радіо-Порт 0, FEC увімкнено)
    WBStreamRx::Options video_options{};
    video_options.radio_port = 0;
    video_options.enable_fec = true;
    auto video_rx = std::make_unique<WBStreamRx>(txrx, video_options);

    // Щойно відео-кадр повністю відновлено з блоків, відправляємо його на GCS
    video_rx->set_callback([video_sock, video_addr](const uint8_t* data, std::size_t size) {
        sendto(video_sock, data, size, 0, (struct sockaddr*)&video_addr, sizeof(video_addr));
    });

    // 3. Демаршрутизація ТЕЛЕМЕТРІЇ (Радіо-Порт 1, FEC вимкнено)
    // FECDisabledDecoder автоматично відстежує sequence_number і відкидає дублікати
    WBStreamRx::Options telemetry_options{};
    telemetry_options.radio_port = 1;
    telemetry_options.enable_fec = false;
    auto telemetry_rx = std::make_unique<WBStreamRx>(txrx, telemetry_options);

    // Щойно отримано унікальний пакет CRSF, миттєво шлемо його на GCS
    telemetry_rx->set_callback([telem_sock, telem_addr](const uint8_t* data, std::size_t size) {
        sendto(telem_sock, data, size, 0, (struct sockaddr*)&telem_addr, sizeof(telem_addr));
    });

    console->info("Listening... Video -> {}:10900 | Telemetry -> {}:2223", target_ip, target_ip);

    // 4. Головний цикл з виводом статистики прийому
    auto lastLog = std::chrono::steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto elapsed = std::chrono::steady_clock::now() - lastLog;
        if (elapsed > std::chrono::seconds(2)) {
            lastLog = std::chrono::steady_clock::now();
            std::cout << txrx->get_rx_stats() << std::endl;
        }
    }

    return 0;
}