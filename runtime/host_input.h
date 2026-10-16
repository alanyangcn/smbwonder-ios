#pragma once
// Host input front end: on-screen touch controls and GameController pads, merged and published to nn::hid as the
// iPhone handheld controller (hid_state.h). Main thread only.
#import <UIKit/UIKit.h>
UIView *wonder_input_controls(void);   // touch controls view (add it below the game surface); starts pad discovery
