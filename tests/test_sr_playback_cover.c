#include "sr_playback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *json="{\"item\":{\"name\":\"Test\",\"album\":{\"images\":[{\"width\":300,\"url\":\"https://example/large.jpg\"},{\"width\":64,\"url\":\"https://example/small.jpg\"}]}}}";
    sr_playback_t playback;
    assert(sr_playback_parse(json,&playback));
    assert(strcmp(playback.cover_url,"https://example/small.jpg")==0);
    puts("cover choice PASS");return 0;
}
