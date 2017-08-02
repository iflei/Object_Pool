#include <iostream>
#include <assert.h>
using namespace std;

typedef void(*Handler)();

//一级空间配置器
template <int inst>
class __MallocAllocTemplate
{
private:
	static Handler __MallocAlloc_oom_Handler; //静态成员需要在类外初始化

	static void* oom_Malloc(size_t n)
	{
		Handler my_malloc_handler;
		void* result;

		while (1)
		{
			my_malloc_handler = __MallocAlloc_oom_Handler;
			if (0 == my_malloc_handler)
			{
				cerr << "out of memory" << endl;
				exit(1);
			}

			my_malloc_handler();
			result = malloc(n);
			if (result)
				return result;
		}
	}

	static void* oom_Realloc(void* p, size_t n)
	{
		Handler my_malloc_handler;
		void* result;

		while (1)
		{
			my_malloc_handler = __MallocAlloc_oom_Handler;
			if (0 == my_malloc_handler)
			{
				cerr << "out of memory" << endl;
				exit(1);
			}

			my_malloc_handler();
			result = realloc(p, n);
			if (result)
				return result;
		}
	}

public:
	static void* Allocate(size_t n)
	{
		void* result = malloc(n);
		if (0 == result)
			result = oom_Malloc(n);
		return result;
	}

	//二级空间配置器调用的时候会传n，这里用不到
	static void Deallocate(void* p, size_t /* n */)
	{
		free(p);
	}

	static void* Reallocate(void* p, size_t /* old_sz */, size_t new_sz)
	{
		void* result = realloc(p, new_sz);
		if (0 == result)
			result = oom_Realloc(p, new_sz);
		return result;
	}

	static Handler SetMallocHandler(Handler f)
	{
		Handler old = __MallocAlloc_oom_Handler;
		__malloc_alloc_oom_handler = f;
		return(old);
	}
};

//静态成员初始化
template <int inst> 
Handler __MallocAllocTemplate<inst>::__MallocAlloc_oom_Handler = 0;

typedef __MallocAllocTemplate<0> MallocAlloc; //一级空间配置器别名MallocAlloc

//二级空间配置器
template <bool threads, int inst>
class __DefaultAllocTemplate
{
private:
	static const int __ALIGN = 8;
	static const int __MAX_BYTES = 128;
	static const int __NFREELISTS = __MAX_BYTES / __ALIGN; //自由链表数

	union Obj {
		union Obj* freeListLink;
		char clientData[1];    /* The client sees this. */
	};

	//
	static char* startFree;
	static char* endFree;
	static size_t heapSize;
	//

	static Obj* freeList[__NFREELISTS]; //自由链表

	//获取自由链表位置
	static size_t FreeListIndex(size_t bytes)
	{
		return ((bytes + __ALIGN - 1) / __ALIGN - 1); //(byte + 7) / 8 -1
	}

	//向8对齐
	static size_t ROUND_UP(size_t n)
	{
		return (((bytes)+__ALIGN - 1) & ~(__ALIGN - 1));
	}

	static void* Refill(size_t n) //没有可用freelist，重新填充freelist，n是块大小
	{
		int nobjs = 20;
		char* chunk = ChunkAlloc(n, nobjs); //尝试取得nobjs个区块

		Obj** myFreeList = NULL;
		Obj* result;

		Obj* cur = NULL;
		Obj* next = NULL;

		//nobjs引用传参的，如果只获得一个区块，就分配给调用者，不给freelist加节点
		if (1 == nobjs)
			return chunk;

		//ChunkAlloc获取了多个区块

		myFreeList = freeList + FreeListIndex(n);
		result = (Obj*)chunk; //把第一个区块返回给调用者

		*myFreeList = next = (Obj*)(chunk + n); //freelist的头指针更新下

		//将free list各节点串起来
		//cur从freelist第一个结点开始遍历
		int i = 1;
		while (i)
		{
			cur = next; 
			next = (Obj*)((char*)cur + n); //(char*)cur + n指向下一块

			if (nobjs - 1 == i) //当i等于当前区块数时
			{
				//遍历到最后，cur指向随后一个区块
				cur->freeListLink = 0;
				return;
			}
			//链接各区块
			cur->freeListLink = next;
			++i;
		}

		return result;
	}

	//内存池
	static char* ChunkAlloc(size_t size, int& nobjs) //size是区块大小，已经调到8的倍数
	{
		char* result = NULL;
		size_t totalBytes = nobjs * size; //总共需要多少字节
		size_t bytesLeft = endFree - startFree; //当前剩余多少字节

		if (bytesLeft >= totalBytes) //内存池剩余空间完全满足需求量
		{
			result = startFree;
			startFree += totalBytes;
			return result;
		}
		else if (bytesLeft >= size) //剩余空间不能完全满足需求，但足够一个以上区块
		{
			nobjs = bytesLeft / size; //nobjs引用传参
			totalBytes = nobjs * size;
			result = startFree;
			startFree += totalBytes;
			return result;
		}
		else //连一个区块都分不出来（内部碎片）
		{
			size_t bytesToGet = 2 * totalBytes + ROUND_UP(heapSize >> 4); //heapSize初始是0，后面更新

			//让内存池的残余零头有价值,配置给小块的free list
			if (bytesLeft > 0) //有剩余
			{
				//把剩下的先分配给适当的free list
				//bytesLeft肯定是8的倍数，内存池增加内存时用malloc开8的倍数。
				Obj** myFreeList = freeList + FreeListIndex(bytesLeft); 
				//头插到合适的free list里
				((Obj*)startFree)->freeListLink = *myFreeList;
				*myFreeList = (Obj*)startFree;
			}

			//startFree已经把零头分配给其他更小的free list了，内存池完全空了
			//任何残余零头终将编入适当的free list中

			//配置heap空间，补充内存池
			startFree = (char*)malloc(bytesToGet);
			if (0 == startFree) //heap空间不足,malloc失败
			{
				Obj** myFreeList = NULL;
				Obj* p = NULL;

				//既然malloc不出空间，那搜寻未用的大区块的free list，把它切割成合适的
				for (int i = size; i <= __MAX_BYTES; i += __ALIGN)
				{
					myFreeList = freeList + FreeListIndex(i);
					p = *myFreeList;

					if (p) //free list的头指针不为空，说明free list里尚有未用区块
					{
						//调整free list 的头指针，释放未用区块
						*myFreeList = p->freeListLink;
						//用free list 的空闲大区块扩充内存池
						startFree = (char*)p;
						endFree = startFree + i; //i是大区块的字节数

						//这时内存池已经通过切割大区块的方式扩充了
						//再调用自己可以开出需要的区块大小，同时会更新实际开了nobjs个区块
						return ChunkAlloc(size, nobjs); //调用自己，修正nobjs
					}
				}

				endFree = 0; //上面的for循环遍历完所有free list 都为空，山穷水尽

				//调用一级空间配置器，看 OOM 可以释放出内存不
				startFree = (char*)MallocAlloc::Allocate(bytesToGet);
			}

			heapSize += bytesToGet;
			endFree = startFree + bytesToGet;
			return ChunkAlloc(size, nobjs); //调用自己，修正nobjs
		}
	}

public:
	static void* Allocate(size_t n)
	{
		Obj** myFreeList = NULL;
		Obj* result = NULL;

		if (n > (size_t)__MAX_BYTES) //n大于128调用一级空间配置器
			return (MallocAlloc::Allocate(n));

		//寻找适合的自由链表
		myFreeList = freeList + FreeListIndex(n);

		result = *myFreeList; //自由链表的头指针，指向自由链表的第一个结点
		if (0 == result) //没有可用的free list
		{
			//重新填充自由链表，Refill返回一个结点
			void* ret = Refill(ROUND_UP(n)); //调整n到8的倍数
			return ret;
		}

		//有可用的free list，调整free list
		*myFreeList = result->freeListLink;
		return result; //链表的头删
	}

	static void Deallocate(void *p, size_t n)
	{
		assert(p);//p不能为0

		Obj* q = (Obj*)p;
		Obj** myFreeList = NULL;

		if (n > (size_t)__MAX_BYTES) //n大于128调用一级空间配置器
		{
			MallocAlloc::Deallocate(p, n);
			return;
		}

		//寻找对应的free list
		myFreeList = freeList + FreeListIndex(n);

		//还回内存块，头插结点
		q->freeListLink = *myFreeList;
		*myFreeList = q;
	}

	
	static void* Reallocate(void *p, size_t old_sz, size_t new_sz)
	{
		void * result;
		size_t copy_sz;

		if (old_sz > (size_t)__MAX_BYTES && new_sz > (size_t)__MAX_BYTES)
			return(realloc(p, new_sz)); //直接调realloc

		//向8对齐相等，说明原来多分配的内存来够用
		if (ROUND_UP(old_sz) == ROUND_UP(new_sz)) 
			return(p);
		
		result = Allocate(new_sz);
		copy_sz = new_sz > old_sz ? old_sz : new_sz;
		memcpy(result, p, copy_sz);
		Deallocate(p, old_sz);
		return(result);
	}
};

//静态成员类外初始化
template <bool threads, int inst> 
size_t __DefaultAllocTemplate<threads, inst>::heapSize = 0; 
template <bool threads, int inst>
char* __DefaultAllocTemplate<threads, inst>::startFree = NULL;
template <bool threads, int inst>
char* __DefaultAllocTemplate<threads, inst>::endFree = NULL;
template <bool threads, int inst>
typename __DefaultAllocTemplate<threads, inst>::Obj* __DefaultAllocTemplate<threads, inst>::freeList[] = { NULL };

 //二级空间配置器别名Alloc
typedef __DefaultAllocTemplate<true, 0> Alloc;


//对空间配置器再封装借口，符合STL标准
template <typename T, typename Alloc>
class SimpleAlloc
{
public:
	static T* Allocate(size_t n)
	{
		return 0 == n ? 0 : (T*)Alloc::Allocate(n * sizeof (T));
	}
	static T* Allocate(void)
	{
		return (T*)Alloc::Allocate(sizeof (T));
	}
	static void Deallocate(T* p, size_t n)
	{
		if (0 != n) 
			Alloc::Deallocate(p, n * sizeof (T));
	}
	static void Deallocate(T* p)
	{
		Alloc::Deallocate(p, sizeof (T));
	}
};