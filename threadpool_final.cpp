// threadpool_final.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include<functional>
#include<thread>
#include <future>

#include"threadpool.h"

using namespace std;


/*
如何让线程池提交任务更方便
pool.submitTask(func，arg..)
1、submitTask：可变参模板编程

2、Result复杂
	c++11  packaged_task(function函数对象)  async 
*/
int func_1(int n)
{
	this_thread::sleep_for(chrono::seconds(2));
	for (int i = 0; i < n; i++)
		n = n + i;
	return n;
}

int func_2(int a, int b)
{	
	int t = 0;
	for (int i = a; i < a + b; i++) {
		t = t + i;
	}	
	return t ;
}
int main()
{
	ThreadPool pool;
	pool.setMode(PoolMode::MODE_CACHED);
	pool.start(2);

	future<int> r1 = pool.submitTask(func_1,100);
	future<int> r2 = pool.submitTask(func_2,1000,2000);

	cout << r1.get() << endl;
	cout << r2.get() << endl;
	//packaged_task<int(int,int)> task(sum1);
	//future<int> res = task.get_future();
	////task(10, 20);

	//thread t(task, 10, 20);
	//t.detach();

	//cout << res.get()<<endl;

}
