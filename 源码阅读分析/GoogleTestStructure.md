# GoogleTest框架
## 测试项目

下面直接在注释中给出具体用法
```C++
TEST(ProjectName, SectionName){
    // 断言 A >= B
    ASSERT_GE(A,B)
    // 断言 A <= B
    ASSERT_LE(A,B)
    // 断言 A == B
    ASSERT_EQ(A,B)
    // 断言 exp == true
    ASSERT_TRUE(exp)

}
```