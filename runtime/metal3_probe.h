#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
// Actual host GPU diagnostic. Does not consume NVN commands or represent a game frame.
NSDictionary *wonder_metal3_run(id<MTLDevice>,NSString *,NSError **);
