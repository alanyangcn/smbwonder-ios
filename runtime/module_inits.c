#include "dm.h"
void wonder_module_inits(module_t *modules,int count) {
    if(count<0||count>MAX_MOD)die("Invalid constructor module count");
    for(int i=0;i<count;i++) {
        module_t *module=&modules[i];
        if(!module->init)continue;
        uint64_t offset=module->init;
        if(offset>=module->size || !(offset<module->text_end || offset>=module->patch_off))die("Invalid executable DT_INIT for %s",module->name);
        if(module->init_arraysz%8 || module->init_array>module->size || module->init_arraysz>module->size-module->init_array)die("Invalid init_array for %s",module->name);
        LOG("CONSTRUCTORS_BEGIN %s DT_INIT=+%#llx entries=%llu",module->name,offset,module->init_arraysz/8);
        // The original DT_INIT registers its DSO and calls init_array itself. Never call the array a second time.
        ((void (*)(void))(module->base+offset))();
        LOG("CONSTRUCTORS_OK %s entries=%llu",module->name,module->init_arraysz/8);
    }
}
