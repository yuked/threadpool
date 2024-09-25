#include <iostream>
#include<functional>
#include<thread>
#include <future>

#include"threadpool.h"

using namespace std;

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
	ThreadPool pool;         //creat threadpool
	pool.setMode(PoolMode::MODE_CACHED); //set cached mode
	pool.start(2);          //start threadpool

	future<int> r1 = pool.submitTask(func_1,100);      //Submit task 1 for execution; r1 will receive the result.
	future<int> r2 = pool.submitTask(func_2,1000,2000);	//Submit task 2 for execution; r2 will receive the result.

	cout << r1.get() << endl;   //output the result r1
	cout << r2.get() << endl;   //output the result r2

	return 0;
}
