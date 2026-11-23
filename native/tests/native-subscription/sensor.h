#ifdef __cplusplus
extern "C" {
#endif

#ifdef SENSOR_BLOCKS
typedef int (^Reading)(int);
unsigned long long subscribe(Reading _Nonnull callback)
    __attribute__((acquire_handle("Subscription")));
#else
typedef int (*Reading)(int, int);
unsigned long long subscribe(Reading _Nonnull callback, int limit)
    __attribute__((acquire_handle("Subscription"), callback(callback, -1, limit)));
#endif
void unsubscribe(unsigned long long token __attribute__((release_handle("Subscription")))) __attribute__((swift_attr("nonisolated")));
int deliver(int slot, int value);
int beginDelivery(int slot);
int finishDelivery(void);
int openCount(void);
int closeCount(void);

#ifdef __cplusplus
}
#endif
