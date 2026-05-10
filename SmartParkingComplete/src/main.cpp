#include <SFML/Graphics.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class CountingSemaphore {
    std::mutex mtx;
    std::condition_variable cv;
    int count;
public:
    explicit CountingSemaphore(int initial) : count(initial) {}
    void acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return count > 0; });
        --count;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mtx);
        ++count;
        cv.notify_one();
    }
};

enum class VehicleType { Normal, VIP, Ambulance };
enum class VehicleState { Waiting, Parked, Completed, Cancelled };

struct Vehicle {
    int id;
    VehicleType type;
    int basePriority;
    VehicleState state = VehicleState::Waiting;
    int slot = -1;
    Clock::time_point arrivalTime;
    Clock::time_point parkStart;
    double waitSeconds = 0.0;
    double parkDuration = 0.0;
};

std::string typeName(VehicleType t) {
    if (t == VehicleType::Ambulance) return "AMBULANCE";
    if (t == VehicleType::VIP) return "VIP";
    return "NORMAL";
}

int basePriority(VehicleType t) {
    if (t == VehicleType::Ambulance) return 1;
    if (t == VehicleType::VIP) return 2;
    return 3;
}

sf::Color vehicleColor(VehicleType t) {
    if (t == VehicleType::Ambulance) return sf::Color(230, 60, 60);
    if (t == VehicleType::VIP) return sf::Color(245, 205, 60);
    return sf::Color(70, 150, 240);
}

struct Snapshot {
    std::vector<Vehicle> vehicles;
    std::vector<int> slots;
    int completed = 0;
    int totalCreated = 0;
    double avgWait = 0.0;
    double throughput = 0.0;
    double utilization = 0.0;
    double elapsed = 0.0;
};

class ParkingManager {
    static constexpr int SLOT_COUNT = 5;
    CountingSemaphore semaphore;
    mutable std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Vehicle>> vehicles;
    std::vector<int> slots;
    std::vector<Clock::time_point> slotStart;
    std::vector<std::thread> threads;
    std::atomic<bool> stopFlag{false};
    Clock::time_point startTime;
    int nextId = 1;
    int completed = 0;
    double totalWait = 0.0;
    double occupiedSeconds = 0.0;

    int effectivePriority(const Vehicle& v, Clock::time_point now) const {
        double waited = std::chrono::duration<double>(now - v.arrivalTime).count();
        int agingBoost = static_cast<int>(waited / 6.0); // every 6 sec improves priority
        int p = v.basePriority - std::min(agingBoost, 2);
        return std::max(1, p);
    }

    bool isBestCandidateLocked(int id, Clock::time_point now) const {
        std::shared_ptr<Vehicle> me;
        for (auto& v : vehicles) {
            if (v->id == id) {
                me = v;
                break;
            }
        }
        if (!me || me->state != VehicleState::Waiting) return false;

        int myPriority = effectivePriority(*me, now);
        for (auto& other : vehicles) {
            if (other->state != VehicleState::Waiting || other->id == id) continue;
            int op = effectivePriority(*other, now);
            if (op < myPriority) return false;
            if (op == myPriority && other->arrivalTime < me->arrivalTime) return false;
        }
        return true;
    }

    bool hasFreeSlotLocked() const {
        return std::any_of(slots.begin(), slots.end(), [](int x) { return x == -1; });
    }

    int getFreeSlotLocked() const {
        for (int i = 0; i < (int)slots.size(); ++i) {
            if (slots[i] == -1) return i;
        }
        return -1;
    }

    void vehicleRoutine(std::shared_ptr<Vehicle> v) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::cout << "Vehicle " << v->id << " (" << typeName(v->type) << ") arrived\n";
        }
        cv.notify_all();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] {
                auto now = Clock::now();
                return stopFlag || (hasFreeSlotLocked() && isBestCandidateLocked(v->id, now));
            });
            if (stopFlag) {
                v->state = VehicleState::Cancelled;
                return;
            }
        }

        semaphore.acquire();

        int allocatedSlot = -1;
        {
            std::lock_guard<std::mutex> lock(mtx);
            allocatedSlot = getFreeSlotLocked();
            if (allocatedSlot == -1) {
                semaphore.release();
                return;
            }
            auto now = Clock::now();
            v->slot = allocatedSlot;
            v->state = VehicleState::Parked;
            v->parkStart = now;
            v->waitSeconds = std::chrono::duration<double>(now - v->arrivalTime).count();
            slots[allocatedSlot] = v->id;
            slotStart[allocatedSlot] = now;
            std::cout << "Vehicle " << v->id << " got Slot " << allocatedSlot + 1
                      << " after waiting " << std::fixed << std::setprecision(2)
                      << v->waitSeconds << " sec\n";
        }
        cv.notify_all();

        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(4, 9);
        int seconds = dist(rng);
        for (int i = 0; i < seconds * 10 && !stopFlag; ++i)
            std::this_thread::sleep_for(100ms);

        {
            std::lock_guard<std::mutex> lock(mtx);
            auto now = Clock::now();
            v->parkDuration = std::chrono::duration<double>(now - v->parkStart).count();
            if (v->slot >= 0 && v->slot < (int)slots.size()) {
                occupiedSeconds += std::chrono::duration<double>(now - slotStart[v->slot]).count();
                slots[v->slot] = -1;
            }
            v->state = VehicleState::Completed;
            ++completed;
            totalWait += v->waitSeconds;
            std::cout << "Vehicle " << v->id << " left parking\n";
        }

        semaphore.release();
        cv.notify_all();
    }

public:
    ParkingManager() : semaphore(SLOT_COUNT), slots(SLOT_COUNT, -1), slotStart(SLOT_COUNT) {
        startTime = Clock::now();
    }

    ~ParkingManager() { shutdown(); }

    void addVehicle(VehicleType type) {
        auto v = std::make_shared<Vehicle>();
        {
            std::lock_guard<std::mutex> lock(mtx);
            v->id = nextId++;
            v->type = type;
            v->basePriority = basePriority(type);
            v->arrivalTime = Clock::now();
            vehicles.push_back(v);
        }
        threads.emplace_back(&ParkingManager::vehicleRoutine, this, v);
        cv.notify_all();
    }

    void shutdown() {
        bool expected = false;
        if (stopFlag.compare_exchange_strong(expected, true)) {
            cv.notify_all();
            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
        }
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mtx);
        Snapshot s;
        auto now = Clock::now();
        s.elapsed = std::chrono::duration<double>(now - startTime).count();
        s.slots = slots;
        s.completed = completed;
        s.totalCreated = (int)vehicles.size();
        s.avgWait = completed == 0 ? 0.0 : totalWait / completed;
        s.throughput = s.elapsed <= 0 ? 0.0 : completed / (s.elapsed / 60.0);

        double currentOccupied = occupiedSeconds;
        for (int i = 0; i < (int)slots.size(); ++i) {
            if (slots[i] != -1) {
                currentOccupied += std::chrono::duration<double>(now - slotStart[i]).count();
            }
        }
        s.utilization = s.elapsed <= 0 ? 0.0 : (currentOccupied / (SLOT_COUNT * s.elapsed)) * 100.0;
        s.utilization = std::min(100.0, s.utilization);

        for (auto& v : vehicles) s.vehicles.push_back(*v);
        return s;
    }
};

bool loadFont(sf::Font& font) {
    const std::vector<std::string> paths = {
        "assets/arial.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        "C:/Windows/Fonts/segoeui.ttf"
    };
    for (const auto& p : paths) {
        if (font.openFromFile(p)) return true;
    }
    return false;
}

void drawText(sf::RenderWindow& window, const sf::Font* font, const std::string& str,
              float x, float y, unsigned size = 18, sf::Color color = sf::Color::White) {
    if (!font) return;
    sf::Text text(*font, str, size);
    text.setFillColor(color);
    text.setPosition({x, y});
    window.draw(text);
}

int main() {
    sf::RenderWindow window(sf::VideoMode({1200, 720}), "Smart Parking Allocation System - OS Project");
    window.setFramerateLimit(60);

    sf::Font font;
    sf::Font* fontPtr = loadFont(font) ? &font : nullptr;
    if (!fontPtr) std::cout << "Font not found. GUI text will be hidden, but simulation still works.\n";

    ParkingManager manager;
    bool heavyMode = false;
    sf::Clock heavyClock;

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) window.close();

            if (const auto* key = event->getIf<sf::Event::KeyPressed>()) {
                if (key->code == sf::Keyboard::Key::Escape) window.close();
                if (key->code == sf::Keyboard::Key::N) manager.addVehicle(VehicleType::Normal);
                if (key->code == sf::Keyboard::Key::V) manager.addVehicle(VehicleType::VIP);
                if (key->code == sf::Keyboard::Key::A) manager.addVehicle(VehicleType::Ambulance);
                if (key->code == sf::Keyboard::Key::H) heavyMode = !heavyMode;
            }
        }

        if (heavyMode && heavyClock.getElapsedTime().asSeconds() > 1.2f) {
            static int pattern = 0;
            if (pattern % 7 == 0) manager.addVehicle(VehicleType::Ambulance);
            else if (pattern % 3 == 0) manager.addVehicle(VehicleType::VIP);
            else manager.addVehicle(VehicleType::Normal);
            ++pattern;
            heavyClock.restart();
        }

        Snapshot snap = manager.snapshot();
        window.clear(sf::Color(22, 26, 32));

        drawText(window, fontPtr, "SMART PARKING ALLOCATION SYSTEM", 30, 20, 28, sf::Color(120, 220, 255));
        drawText(window, fontPtr, "OS Concepts: Threads | Mutex | Counting Semaphore | Priority Scheduling | Aging", 30, 58, 17);
        drawText(window, fontPtr, "Controls: N=Normal  V=VIP  A=Ambulance  H=Heavy Load ON/OFF  Esc=Exit", 30, 88, 16, sf::Color(220, 220, 220));

        drawText(window, fontPtr, "Parking Slots", 50, 135, 24, sf::Color(255, 255, 255));
        for (int i = 0; i < (int)snap.slots.size(); ++i) {
            float x = 55 + i * 150;
            float y = 180;
            sf::RectangleShape slot({120, 90});
            slot.setPosition({x, y});
            slot.setFillColor(snap.slots[i] == -1 ? sf::Color(45, 55, 65) : sf::Color(80, 120, 85));
            slot.setOutlineColor(sf::Color(180, 180, 180));
            slot.setOutlineThickness(2);
            window.draw(slot);

            drawText(window, fontPtr, "Slot " + std::to_string(i + 1), x + 25, y + 10, 16);
            if (snap.slots[i] != -1) {
                VehicleType t = VehicleType::Normal;
                for (const auto& v : snap.vehicles) if (v.id == snap.slots[i]) t = v.type;
                sf::CircleShape car(25);
                car.setFillColor(vehicleColor(t));
                car.setPosition({x + 35, y + 42});
                window.draw(car);
                drawText(window, fontPtr, std::to_string(snap.slots[i]), x + 50, y + 50, 16, sf::Color::Black);
            } else {
                drawText(window, fontPtr, "EMPTY", x + 30, y + 48, 15, sf::Color(170, 170, 170));
            }
        }

        drawText(window, fontPtr, "Waiting Queue", 50, 320, 24);
        int waitingIndex = 0;
        for (const auto& v : snap.vehicles) {
            if (v.state != VehicleState::Waiting) continue;
            float x = 55 + (waitingIndex % 12) * 70;
            float y = 365 + (waitingIndex / 12) * 70;
            sf::CircleShape car(23);
            car.setFillColor(vehicleColor(v.type));
            car.setPosition({x, y});
            window.draw(car);
            drawText(window, fontPtr, std::to_string(v.id), x + 11, y + 9, 14, sf::Color::Black);
            ++waitingIndex;
        }
        if (waitingIndex == 0) drawText(window, fontPtr, "No vehicle waiting", 55, 365, 18, sf::Color(150, 150, 150));

        sf::RectangleShape panel({330, 390});
        panel.setPosition({835, 150});
        panel.setFillColor(sf::Color(35, 40, 48));
        panel.setOutlineColor(sf::Color(100, 180, 220));
        panel.setOutlineThickness(2);
        window.draw(panel);

        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        drawText(window, fontPtr, "Live Performance Metrics", 860, 175, 22, sf::Color(120, 220, 255));
        drawText(window, fontPtr, "Vehicles Created: " + std::to_string(snap.totalCreated), 860, 225, 18);
        drawText(window, fontPtr, "Completed: " + std::to_string(snap.completed), 860, 255, 18);
        drawText(window, fontPtr, "Waiting: " + std::to_string(waitingIndex), 860, 285, 18);
        out.str(""); out << snap.avgWait << " sec";
        drawText(window, fontPtr, "Average Wait: " + out.str(), 860, 325, 18);
        out.str(""); out << snap.throughput << " vehicles/min";
        drawText(window, fontPtr, "Throughput: " + out.str(), 860, 355, 18);
        out.str(""); out << snap.utilization << "%";
        drawText(window, fontPtr, "Slot Utilization: " + out.str(), 860, 385, 18);
        drawText(window, fontPtr, "Heavy Load: " + std::string(heavyMode ? "ON" : "OFF"), 860, 425, 18, heavyMode ? sf::Color::Green : sf::Color(220, 220, 220));

        drawText(window, fontPtr, "Priority Colors", 860, 470, 18, sf::Color(255, 255, 255));
        sf::CircleShape n(12); n.setFillColor(vehicleColor(VehicleType::Normal)); n.setPosition({865, 505}); window.draw(n);
        drawText(window, fontPtr, "Normal", 895, 500, 16);
        sf::CircleShape vip(12); vip.setFillColor(vehicleColor(VehicleType::VIP)); vip.setPosition({865, 535}); window.draw(vip);
        drawText(window, fontPtr, "VIP", 895, 530, 16);
        sf::CircleShape amb(12); amb.setFillColor(vehicleColor(VehicleType::Ambulance)); amb.setPosition({865, 565}); window.draw(amb);
        drawText(window, fontPtr, "Ambulance", 895, 560, 16);

        drawText(window, fontPtr, "Aging: long-waiting vehicles get priority boost", 50, 645, 17, sf::Color(210, 210, 210));
        drawText(window, fontPtr, "Console also prints allocation log for report/demo proof.", 50, 670, 15, sf::Color(170, 170, 170));

        window.display();
    }

    manager.shutdown();
    return 0;
}
