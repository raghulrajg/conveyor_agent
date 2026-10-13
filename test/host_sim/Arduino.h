#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <functional>
extern unsigned long g_millis;
inline unsigned long millis() { return g_millis; }
inline void delay(unsigned long) {}
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
enum { A0=0,A1,A2,A3,A4,A5, D0=10,D1,D2,D3, LED_D0=20,LED_D1,LED_D2,LED_D3,LED_USER,LEDR,LEDG,BTN_USER };
extern int g_pins[64];
inline int digitalRead(int p) { return g_pins[p]; }
inline void digitalWrite(int p, int v) { if (p >= 10) g_pins[p] = v; }
inline void pinMode(int, int) {}
struct Print {
  std::string out;
  size_t print(const char* s) { out += s; fputs(s, stdout); return strlen(s); }
  size_t print(char c) { char b[2] = {c, 0}; return print(b); }
  size_t println(const char* s) { print(s); return print("\n"); }
  size_t println() { return print("\n"); }
  template<class T> size_t print(const T&) { return print("<obj>"); }
  template<class T> size_t println(const T& t) { print(t); return print("\n"); }
};
struct SerialT : Print { std::string in; void begin(long) {} int available() { return (int)in.size(); }
  int read() { if (in.empty()) return -1; int c = (unsigned char)in[0]; in.erase(0,1); return c; } };
extern SerialT Serial;
struct String { std::string s; String(const char* c = "") : s(c) {} bool operator!=(const char* o) const { return s != o; } };
struct IPAddress { uint8_t b[4]; IPAddress(int a=0,int c=0,int d=0,int e=0) { b[0]=a;b[1]=c;b[2]=d;b[3]=e; } uint8_t operator[](int i) const { return b[i]; } };
struct Client {};
