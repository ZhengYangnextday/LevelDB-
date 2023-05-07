class MyInt {
public:
    explicit MyInt(int n) : num(n) {}

private:
    unsigned int num;
};

int main(){
    MyInt i = 48;
    return 0;
}