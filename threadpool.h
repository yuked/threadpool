#ifndef THREADPOOL_H		//防止头文件被多次包含
#define THREADPOOL_H		//定义宏不赋值	

#include<iostream>
#include<vector>
#include<queue>
#include<memory>   //智能指针
#include<mutex>
#include<condition_variable>
#include<atomic>
#include<thread>
#include<functional>
#include<unordered_map>
#include<future>

const int TASK_MAX_THRESHOLD = 2; // INT_MAX;
const int TRHEAD_MAX_THRESHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;	//线程最大空闲时间

//线程池支持的模式
enum class PoolMode
{
	MODE_FIXED,		//固定
	MODE_CACHED,	//可动态增长
};

//线程类型
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;  //不接收参数，返回值为void,cached模式接受参数threadid

	//线程构造
	Thread(ThreadFunc func) :
		func_(func)
		, threadId_(generateId_++)
	{}

	//线程析构
	~Thread() = default;

	//启动线程
	void start()
	{
		//执行一个函数（线程池提供）
		std::thread t(func_, threadId_);   //对C++11来说 线程对象t 和线程函数func_
		t.detach();  //设置分离线程，func_与t无关
	}

	//获取threadId_
	int getId() const
	{
		return threadId_;
	}

private:
	ThreadFunc func_;    //接受线程池传入的函数对象
	static int generateId_;
	int threadId_;   //方便cached模式下，threadFunc中进行线程回收
};

int Thread::generateId_ = 0;

//线程类型
class ThreadPool
{
public:
	//构造
	ThreadPool()
		: initThreadSize_(0)  //后续会函数调用会初始化，这里无所谓
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, threadSizeThreshold_(TRHEAD_MAX_THRESHOLD)
		, taskQueMaxThreshold_(TASK_MAX_THRESHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	//析构
	~ThreadPool()
	{
		isPoolRunning_ = false;

		//等待线程池中所有线程返回  两种状态： 阻塞 、 正在执行任务
		std::unique_lock<std::mutex> lock(taskQueMtx_);			//等所有线程结束，我们结束main然后调用析构

		notEmpty_.notify_all();     //等待状态 ——>阻塞状态
		//任务执行完，所有线程都阻塞在notEmpty上，wait notEmpty 等待非空
		exitCond_.wait(lock, [&]() {return threads_.size() == 0; });    //等待线程队列为0
	}

	ThreadPool(const ThreadPool&) = delete;   //拷贝构造
	ThreadPool& operator=(const ThreadPool&) = delete;  //拷贝赋值

	//设置工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())   //若已经启动，则不能调用
			return;
		poolMode_ = mode;
	}

	//设置任务队列中数量阈值
	void setTaskQueMaxThreshold(int threshold)
	{
		taskQueMaxThreshold_ = threshold;
	}

	//设置线程池cached模式下线程数量阈值
	void setThreadSizeThreshold(int threshold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
			threadSizeThreshold_ = threshold;
	}

	//使用可变参模板编程，让submitTask接受任意函数和任意数量的参数  
	//右值引用＋引用折叠
	//返回值future<>
	template<typename Func, typename...Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>{//完美转发参数和万能引用//打包任务，放入任务队列
		using RType = decltype(func(args...));	 //RType为一种数据类型,给类型重命名
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
			//std::forward完美转化，让func和args...保持原有状态（左值引用/右值引用）
			//std::bind用于创建一个function对象.并且已经传入了参数，
			std::future<RType> result = task->get_future();

			//获取锁
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			//线程的通信   条件变量，等待任务队列有空余
			if(!notFull_.wait_for(lock, std::chrono::seconds(1),
				[&]() {return taskQue_.size() < taskQueMaxThreshold_; }))	//用户提交任务，最长不能阻塞超过1s，否则判定提交任务失败，返回
			{	  //wait一直等，知道后面是1才继续执行
				//wait_for 返回一个布尔值，表示条件是否满足。如果在 1 秒内条件满足，返回 true；
				//如果超时且条件仍未满足，返回 false。
				std::cerr << "task queue is full, submit task fail." << std::endl;

				auto task = std::make_shared<std::packaged_task<RType()>>(
					[]()->RType {return RType(); });
				//返回一个 RType 类型的默认构造对象。
				//return task->getResult();  //执行完task就被析构了
				(*task)();
				return task->get_future();
			}
			//如果有空余，将任务放入任务队列
			taskQue_.emplace([task]() {(* task)(); });  //无参数无返回值的匿名函数对象(lambda表达式)
			//*task解引用得到packaged_task类
			//将一个新的任务添加到 taskQue_ 中，这个任务的定义是一个 Lambda 表达式，当这个 Lambda 被调用时，它会执行(*task)()，即执行之前封装在 task 中的任务。
			taskSize_++;

			//因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知
			notEmpty_.notify_all();

			//cached模式，任务处理比较紧急  场景：小而快的任务，需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
			if (poolMode_ == PoolMode::MODE_CACHED
				&& taskSize_ > idleThreadSize_		//任务数量 > 空闲线程数量
				&& curThreadSize_ < threadSizeThreshold_)	//总数量 < 阈值
			{
				std::cout << ">>> creat new thread..." << std::this_thread::get_id() << std::endl;
				//创建新线程对象 并 启动
				auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
				int threadId = ptr->getId();
				threads_.emplace(threadId, std::move(ptr));
				//启动
				threads_[threadId]->start();
				//修改线程个数相关变量
				curThreadSize_++;
				idleThreadSize_++;
			}
			//返回任务的Result对象
			return result;
	}

	//开启线程池
	void start(int initThreadSize)
	{
		isPoolRunning_ = true;//设置线程池的运行状态
		initThreadSize_ = initThreadSize;  //记录初始线程个数
		curThreadSize_ = initThreadSize;   //记录线程总数

		//创建线程对象
		for (int i = 0; i < initThreadSize_; i++)  //创建thread线程对象的时候，把线程函数给到thread线程对象//将一个对象的函数threadFucn传递给另一个类，需要将threadFunc与this指针（ThreadPool对象）bind
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));//std::绑定成员函数要求 第一个参数是成员函数指针，第二个是对象地址，返回一个function对象作为Thread构造函数的输入，创建一个Thread对象
			//cached模数需要threadid作为参数传入threadFunc，此处需要std::placeholders::_1作为参数占位符
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr)); //不可以当做左值引用传入，只能当做右值引用进行资源转移
			//threads_.emplac_back(std::move(ptr));  
		}

		//启动多有线程 std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();     //i就是threadId_，刚好我们是从0开始设置的
			idleThreadSize_++;       //开始启动时都是空闲的
		}
	}


private:
	void threadFunc(int threadid) //定义线程函数 从任务队列中消费任务
	{
		//放thread类中不能调用threadpool中成员变量

		auto lastTime = std::chrono::high_resolution_clock().now();

		//所有任务必须执行完成，线程池才可以回收所有线程资源
		for (;;)
		{
			Task task;
			{
				//获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid: " << std::this_thread::get_id()
					<< "尝试获取任务... " << std::endl;
				//cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s,应该吧多余的线程结束回收掉？
				//结束回收（超过initThreadSize_数量的线程需要回收）
				//当前时间 - 上一次线程执行的时间 > 60s 则回收
				//锁＋双重判断
				while (taskQue_.size() == 0)  //任务队列空的话，等等
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);		//回收
						std::cout << "threadid: " << std::this_thread::get_id() << " exit!"
							<< std::endl;
						exitCond_.notify_all();
						return;  //线程函数结束
					}
					if (poolMode_ == PoolMode::MODE_CACHED) //cached模式
					{
						if (std::cv_status::timeout ==	//条件变量，超时返回
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME	  //空闲时间过久
								&& curThreadSize_ > initThreadSize_)  //多余的线程
							{
								//开始回收当前线程
								//记录线程数量的相关变量的值修改
								//把线程对象从线程列表中删除  不知道threadFunc对应的是哪个thread对象
								//threadid => thread对象删除
								threads_.erase(threadid);		//回收
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid: " << std::this_thread::get_id() << " exit!"
									<< std::endl;
								return;
							}
						}
					}
					else //fixed模式
					{	//等待notEmpty条件
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;  //线程被消耗1个

				std::cout << "tid: " << std::this_thread::get_id()
					<< "获取任务成功 " << std::endl;

				//取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();     //析构了
				taskSize_--;

				//如果依然有剩余任务，通知其他的线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				//取出一个任务，进行通知,通知可以继续提交生产任务
				notFull_.notify_all();
			}//释放锁

			//当前线程负责执行这个任务
			if (task != nullptr)
			{
				//task->run();   //执行任务，并将任务返回值setVal给到Result
				//task->exec();  //封装一下
				task();  //执行function<void()>
			}

			idleThreadSize_++;  //执行完任务，空闲线程+1，继续循环
			lastTime = std::chrono::high_resolution_clock().now();  //更新线程执行为任务的时间
		}
	}

	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;	//线程列表, (id,thread)
	int initThreadSize_;			//初始线程数量
	std::atomic_int curThreadSize_; //记录当前线程池中线程总数量
	int threadSizeThreshold_;		//线程数量上限阈值
	std::atomic_int idleThreadSize_;//记录空闲线程的数量

	using Task = std::function<void()>;   
	std::queue<Task> taskQue_;       //任务列表，queue保存，基类指针多态，考虑临时的任务对象，所以用智能指针，run()执行完析构
	std::atomic_uint taskSize_;  //任务数量
	int taskQueMaxThreshold_;    //任务队列中数量阈值

	std::mutex taskQueMtx_;      //保证人物队列的线程安全
	std::condition_variable notFull_;		//任务队列不满
	std::condition_variable notEmpty_;		//任务队列不空
	std::condition_variable exitCond_;      //等到线程资源全部回收

	PoolMode poolMode_;
	std::atomic_bool isPoolRunning_;//当前线程池的启动状态

	
};


#endif