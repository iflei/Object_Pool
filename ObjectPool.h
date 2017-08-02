#pragma once

template<class Object>
class ObjectPool
{
public:
	//构造链表
	ObjectPool(size_t initObjs = 16, size_t maxObjs = 1024)
		: _initObjs(initObjs)
		, _maxObjs(maxObjs)
		, _lastDelete(NULL)
	{
		_head = _tail = new Node(initObjs);
		_useInCount = 0;
	}

	//析构链表
	~ObjectPool()
	{
		Node* cur = _head;
		while (cur)
		{
			Node* next = cur->next;
			delete cur;
			cur = next;
		}
	}

	template<typename Value>
	Object* GetObj(const Value& val)
	{
		void* obj = Allocate();//取一块对象空间
		return new(obj) Object(val); 
		//new定位表达式会调用 Object 的构造函数
	}

	void RetObj(Object* ptr)
	{
		if (ptr)
		{
			ptr->~Object(); //调用对象的析构函数
			Deallocate(ptr);
		}
	}

private:
	//链表节点结构
	struct Node
	{
		void* memory;   // 指向大块内存的指针
		size_t n;		// 当前节点里面存n个对象
		Node* next;		// 指向下一个节点

		Node(size_t nobjs)
		{
			n = nobjs;
			//只开辟空间不构造
			memory = ::operator new(GetObjSize()*n);
			next = NULL;
		}

		~Node()
		{
			::operator delete(memory);
			memory = NULL;
		}
	};

	//兼容64位系统,最小8字节
	inline static size_t GetObjSize()
	{
		return sizeof(Object) > sizeof(Object*) ? sizeof(Object) : sizeof(Object*);
	}

	//开辟新节点
	void AllocNewNode()
	{
		size_t n = _tail->n * 2; //对象个数增加到2倍
		if (n > _maxObjs)
			n = _maxObjs; // n 最多_maxObjs

		Node* node = new Node(n);
		_tail->next = node;
		_tail = node;

		//每开辟一个新节点把_useInCount置0
		_useInCount = 0;
	}

	// O(1)获取对象空间
	void* Allocate()
	{
		// 1.优先使用释放回来的对象
		if (_lastDelete) //_lastDelete链表不为空
		{
			void* obj = _lastDelete;
			_lastDelete = *((Object**)_lastDelete);//这是一个头删操作
			//这里不能直接 *_lastDelete 因为这样是访问一个Object对象
			return obj;
		}

		// 2.到Node里面获取对象
		if (_useInCount >= _tail->n)
			AllocNewNode();

		void* obj = (char*)_tail->memory + _useInCount * GetObjSize();
		_useInCount++;
		return obj;
	}

	//回收对象空间
	void Deallocate(void* ptr)
	{
		//相当于头插一个被回收的对象的地址
		*(Object**)ptr = _lastDelete; //*(Object**)ptr 就是一个Object* 类型的指针
		//这里不能写成 (Object*)ptr = _lastDelete ，因为ptr是个临时值

		_lastDelete = (Object*)ptr;
	}

private:
	size_t _initObjs; //构造时节点里对象个数
	size_t _maxObjs;  //节点里最大对象个数
	Node* _head; //维护链表
	Node* _tail;
	size_t _useInCount; //memory里使用的对象数
	Object* _lastDelete; 
	//实际上是一个链表，_lastDelete是链表的头
	//存之前回收的对象的地址
};