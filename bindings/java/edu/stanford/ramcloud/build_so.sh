c++ -g -Wall -O3 -shared -fPIC -std=c++0x -I/usr/lib/jvm/default-java/include/ -I${HOME}/ramcloud/src/ -I${HOME}/ramcloud/obj.master/ -I${HOME}/ramcloud/logcabin/ -I${HOME}/ramcloud/gtest/include/ -L${HOME}/ramcloud/obj.master -o libedu_stanford_ramcloud_JRamCloud.so edu_stanford_ramcloud_JRamCloud.cc -lramcloud

