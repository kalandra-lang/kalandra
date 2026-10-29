#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=3)return 2;auto d=MTLCreateSystemDefaultDevice();NSError*e=nil;
 auto l=[d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];if(!l)return 3;
 auto f=[l newFunctionWithName:@(argv[2])];if(!f)return 4;
 MTLComputePipelineReflection*r=nil;auto p=[d newComputePipelineStateWithFunction:f options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&r error:&e];if(!p)return 5;
 NSMutableArray*rows=[NSMutableArray new];for(MTLArgument*a in r.arguments)[rows addObject:@{@"name":a.name,@"index":@(a.index),@"access":@(a.access),@"active":@(a.active)}];
 NSData*data=[NSJSONSerialization dataWithJSONObject:rows options:NSJSONWritingPrettyPrinted error:&e];puts([[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
}}
