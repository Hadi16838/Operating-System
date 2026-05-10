SMART PARKING ALLOCATION SYSTEM - OS PROJECT

Features:
- SFML GUI visualization
- Multithreaded vehicle simulation
- Mutex-protected shared parking state
- Counting semaphore for limited slots
- Priority scheduling: Ambulance > VIP > Normal
- Aging/fairness: vehicles waiting too long get priority boost
- Live metrics: average waiting time, throughput, slot utilization

MSYS2 UCRT64 BUILD COMMAND:
cd /f/OS_Project/SmartParking

g++ src/main.cpp -o SmartParking.exe -std=c++20 -lsfml-graphics -lsfml-window -lsfml-system

RUN:
./SmartParking.exe

Controls:
N = Add Normal vehicle
V = Add VIP vehicle
A = Add Ambulance
H = Toggle heavy-load auto generation
Esc = Exit

Recommended folder:
F:\OS_Project\SmartParking

Important:
Open only MSYS2 UCRT64 terminal, not CLANG64/MINGW64/MSYS.
