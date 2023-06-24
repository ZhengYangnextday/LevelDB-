#include<iostream>
#define MAX(a, b) (!!((a) > (b)) ? (a) : (b))
/**
 * @brief 
 * 在上面的示例中，"!!" 宏定义被用来实现 MAX 宏定义，
 * 它将比较操作的结果转换为布尔值，然后根据结果返回相应的值。
 * 然而，由于宏定义中的参数可能会被多次求值，
 * 因此在这个例子中，MAX 宏定义会导致变量 x 和 y 被修改多次，
 * 这可能会导致预期之外的结果。因此，在使用 "!!" 宏定义时需要格外小心。
 * @return int 
 */
int main() {
  int x = 1, y = 2;
  int z = MAX(x++, y++);
  // z is now 3, but x and y have been modified
  printf("x is %d, y is %d, z is %d\n", x, y, z);
  return 0;
}