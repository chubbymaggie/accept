#include <enerc.h>

struct mystruct {
    APPROX int field;
};

class myclass {
public:
  int meth(APPROX int *x);
};

class superclass {
public:
  virtual void meth(APPROX int* x) = 0;
};
class subclass : public superclass {
public:
  void meth(APPROX int* x) {}
};

int main() {
    APPROX int x = 2;
    struct mystruct s;
    s.field = 5;
    struct mystruct s2 = s;
    struct mystruct s3 = {5};

    myclass obj;
    obj.meth(&x);

    new subclass();
}
