#ifndef THREADPOOL_H		//��ֹͷ�ļ�����ΰ���
#define THREADPOOL_H		//����겻��ֵ	

#include<iostream>
#include<vector>
#include<queue>
#include<memory>   //����ָ��
#include<mutex>
#include<condition_variable>
#include<atomic>
#include<thread>
#include<functional>
#include<unordered_map>
#include<future>

const int TASK_MAX_THRESHOLD = 2; // INT_MAX;
const int TRHEAD_MAX_THRESHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;	//�߳�������ʱ��

//�̳߳�֧�ֵ�ģʽ
enum class PoolMode
{
	MODE_FIXED,		//�̶�
	MODE_CACHED,	//�ɶ�̬����
};

//�߳�����
class Thread
{
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(int)>;  //�����ղ���������ֵΪvoid,cachedģʽ���ܲ���threadid

	//�̹߳���
	Thread(ThreadFunc func) :
		func_(func)
		, threadId_(generateId_++)
	{}

	//�߳�����
	~Thread() = default;

	//�����߳�
	void start()
	{
		//ִ��һ���������̳߳��ṩ��
		std::thread t(func_, threadId_);   //��C++11��˵ �̶߳���t ���̺߳���func_
		t.detach();  //���÷����̣߳�func_��t�޹�
	}

	//��ȡthreadId_
	int getId() const
	{
		return threadId_;
	}

private:
	ThreadFunc func_;    //�����̳߳ش���ĺ�������
	static int generateId_;
	int threadId_;   //����cachedģʽ�£�threadFunc�н����̻߳���
};

int Thread::generateId_ = 0;

//�߳�����
class ThreadPool
{
public:
	//����
	ThreadPool()
		: initThreadSize_(0)  //�����ắ�����û��ʼ������������ν
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, threadSizeThreshold_(TRHEAD_MAX_THRESHOLD)
		, taskQueMaxThreshold_(TASK_MAX_THRESHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	//����
	~ThreadPool()
	{
		isPoolRunning_ = false;

		//�ȴ��̳߳��������̷߳���  ����״̬�� ���� �� ����ִ������
		std::unique_lock<std::mutex> lock(taskQueMtx_);			//�������߳̽��������ǽ���mainȻ���������

		notEmpty_.notify_all();     //�ȴ�״̬ ����>����״̬
		//����ִ���꣬�����̶߳�������notEmpty�ϣ�wait notEmpty �ȴ��ǿ�
		exitCond_.wait(lock, [&]() {return threads_.size() == 0; });    //�ȴ��̶߳���Ϊ0
	}

	ThreadPool(const ThreadPool&) = delete;   //��������
	ThreadPool& operator=(const ThreadPool&) = delete;  //������ֵ

	//���ù���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())   //���Ѿ����������ܵ���
			return;
		poolMode_ = mode;
	}

	//�������������������ֵ
	void setTaskQueMaxThreshold(int threshold)
	{
		taskQueMaxThreshold_ = threshold;
	}

	//�����̳߳�cachedģʽ���߳�������ֵ
	void setThreadSizeThreshold(int threshold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
			threadSizeThreshold_ = threshold;
	}

	//ʹ�ÿɱ��ģ���̣���submitTask�������⺯�������������Ĳ���  
	//��ֵ���ã������۵�
	//����ֵfuture<>
	template<typename Func, typename...Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>{//����ת����������������//������񣬷����������
		using RType = decltype(func(args...));	 //RTypeΪһ����������,������������
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
			//std::forward����ת������func��args...����ԭ��״̬����ֵ����/��ֵ���ã�
			//std::bind���ڴ���һ��function����.�����Ѿ������˲�����
			std::future<RType> result = task->get_future();

			//��ȡ��
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			//�̵߳�ͨ��   �����������ȴ���������п���
			if(!notFull_.wait_for(lock, std::chrono::seconds(1),
				[&]() {return taskQue_.size() < taskQueMaxThreshold_; }))	//�û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
			{	  //waitһֱ�ȣ�֪��������1�ż���ִ��
				//wait_for ����һ������ֵ����ʾ�����Ƿ����㡣����� 1 �����������㣬���� true��
				//�����ʱ��������δ���㣬���� false��
				std::cerr << "task queue is full, submit task fail." << std::endl;

				auto task = std::make_shared<std::packaged_task<RType()>>(
					[]()->RType {return RType(); });
				//����һ�� RType ���͵�Ĭ�Ϲ������
				//return task->getResult();  //ִ����task�ͱ�������
				(*task)();
				return task->get_future();
			}
			//����п��࣬����������������
			taskQue_.emplace([task]() {(* task)(); });  //�޲����޷���ֵ��������������(lambda���ʽ)
			//*task�����õõ�packaged_task��
			//��һ���µ�������ӵ� taskQue_ �У��������Ķ�����һ�� Lambda ���ʽ������� Lambda ������ʱ������ִ��(*task)()����ִ��֮ǰ��װ�� task �е�����
			taskSize_++;

			//��Ϊ�·�������������п϶������ˣ���notEmpty_�Ͻ���֪ͨ
			notEmpty_.notify_all();

			//cachedģʽ��������ȽϽ���  ������С�����������Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
			if (poolMode_ == PoolMode::MODE_CACHED
				&& taskSize_ > idleThreadSize_		//�������� > �����߳�����
				&& curThreadSize_ < threadSizeThreshold_)	//������ < ��ֵ
			{
				std::cout << ">>> creat new thread..." << std::this_thread::get_id() << std::endl;
				//�������̶߳��� �� ����
				auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
				int threadId = ptr->getId();
				threads_.emplace(threadId, std::move(ptr));
				//����
				threads_[threadId]->start();
				//�޸��̸߳�����ر���
				curThreadSize_++;
				idleThreadSize_++;
			}
			//���������Result����
			return result;
	}

	//�����̳߳�
	void start(int initThreadSize)
	{
		isPoolRunning_ = true;//�����̳߳ص�����״̬
		initThreadSize_ = initThreadSize;  //��¼��ʼ�̸߳���
		curThreadSize_ = initThreadSize;   //��¼�߳�����

		//�����̶߳���
		for (int i = 0; i < initThreadSize_; i++)  //����thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���//��һ������ĺ���threadFucn���ݸ���һ���࣬��Ҫ��threadFunc��thisָ�루ThreadPool����bind
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));//std::�󶨳�Ա����Ҫ�� ��һ�������ǳ�Ա����ָ�룬�ڶ����Ƕ����ַ������һ��function������ΪThread���캯�������룬����һ��Thread����
			//cachedģ����Ҫthreadid��Ϊ��������threadFunc���˴���Ҫstd::placeholders::_1��Ϊ����ռλ��
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr)); //�����Ե�����ֵ���ô��룬ֻ�ܵ�����ֵ���ý�����Դת��
			//threads_.emplac_back(std::move(ptr));  
		}

		//���������߳� std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();     //i����threadId_���պ������Ǵ�0��ʼ���õ�
			idleThreadSize_++;       //��ʼ����ʱ���ǿ��е�
		}
	}


private:
	void threadFunc(int threadid) //�����̺߳��� �������������������
	{
		//��thread���в��ܵ���threadpool�г�Ա����

		auto lastTime = std::chrono::high_resolution_clock().now();

		//�����������ִ����ɣ��̳߳زſ��Ի��������߳���Դ
		for (;;)
		{
			Task task;
			{
				//��ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid: " << std::this_thread::get_id()
					<< "���Ի�ȡ����... " << std::endl;
				//cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s,Ӧ�ðɶ�����߳̽������յ���
				//�������գ�����initThreadSize_�������߳���Ҫ���գ�
				//��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60s �����
				//����˫���ж�
				while (taskQue_.size() == 0)  //������пյĻ����ȵ�
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);		//����
						std::cout << "threadid: " << std::this_thread::get_id() << " exit!"
							<< std::endl;
						exitCond_.notify_all();
						return;  //�̺߳�������
					}
					if (poolMode_ == PoolMode::MODE_CACHED) //cachedģʽ
					{
						if (std::cv_status::timeout ==	//������������ʱ����
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME	  //����ʱ�����
								&& curThreadSize_ > initThreadSize_)  //������߳�
							{
								//��ʼ���յ�ǰ�߳�
								//��¼�߳���������ر�����ֵ�޸�
								//���̶߳�����߳��б���ɾ��  ��֪��threadFunc��Ӧ�����ĸ�thread����
								//threadid => thread����ɾ��
								threads_.erase(threadid);		//����
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid: " << std::this_thread::get_id() << " exit!"
									<< std::endl;
								return;
							}
						}
					}
					else //fixedģʽ
					{	//�ȴ�notEmpty����
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;  //�̱߳�����1��

				std::cout << "tid: " << std::this_thread::get_id()
					<< "��ȡ����ɹ� " << std::endl;

				//ȡһ���������
				task = taskQue_.front();
				taskQue_.pop();     //������
				taskSize_--;

				//�����Ȼ��ʣ������֪ͨ�������߳�ִ������
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				//ȡ��һ�����񣬽���֪ͨ,֪ͨ���Լ����ύ��������
				notFull_.notify_all();
			}//�ͷ���

			//��ǰ�̸߳���ִ���������
			if (task != nullptr)
			{
				//task->run();   //ִ�����񣬲������񷵻�ֵsetVal����Result
				//task->exec();  //��װһ��
				task();  //ִ��function<void()>
			}

			idleThreadSize_++;  //ִ�������񣬿����߳�+1������ѭ��
			lastTime = std::chrono::high_resolution_clock().now();  //�����߳�ִ��Ϊ�����ʱ��
		}
	}

	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;	//�߳��б�, (id,thread)
	int initThreadSize_;			//��ʼ�߳�����
	std::atomic_int curThreadSize_; //��¼��ǰ�̳߳����߳�������
	int threadSizeThreshold_;		//�߳�����������ֵ
	std::atomic_int idleThreadSize_;//��¼�����̵߳�����

	using Task = std::function<void()>;   
	std::queue<Task> taskQue_;       //�����б�queue���棬����ָ���̬��������ʱ�������������������ָ�룬run()ִ��������
	std::atomic_uint taskSize_;  //��������
	int taskQueMaxThreshold_;    //���������������ֵ

	std::mutex taskQueMtx_;      //��֤������е��̰߳�ȫ
	std::condition_variable notFull_;		//������в���
	std::condition_variable notEmpty_;		//������в���
	std::condition_variable exitCond_;      //�ȵ��߳���Դȫ������

	PoolMode poolMode_;
	std::atomic_bool isPoolRunning_;//��ǰ�̳߳ص�����״̬

	
};


#endif