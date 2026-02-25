#pragma once

template<typename T>
struct DeferExitScope {
  T lambda;

  DeferExitScope(T l) : lambda(l) {}

  ~DeferExitScope() { lambda(); }
  DeferExitScope(const DeferExitScope&);

  operator bool() const { return true; }

private:
  DeferExitScope& operator = (const DeferExitScope&);
};

class DeferExitScopeHelp {
public:
  template<typename T>
  DeferExitScope<T> operator+(T t) { return t; }
};

#define COMBINE1(X,Y) X##Y
#define CMB(X,Y) COMBINE1(X,Y)
#define defer const auto & CMB(defer__, __COUNTER__) = DeferExitScopeHelp() + [&]()
