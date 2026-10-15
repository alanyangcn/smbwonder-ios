#pragma once
#include <stddef.h>
// Profile picture of the offline local user (nn::account::LoadProfileImage): the host has no Nintendo user icon, so a
// deterministic 256x256 JPEG placeholder is generated once (a disclosed functional difference, like the system fonts).
const unsigned char *wonder_profile_image(size_t *size);   // process-lifetime bytes; NULL if generation failed
