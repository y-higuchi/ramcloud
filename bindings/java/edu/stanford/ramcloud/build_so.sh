JAVA_HOME=/usr/lib/jvm/java-7-oracle
RAMCLOUD_HOME=~/ramcloud
RAMCLOUD_BRANCH=blueprint-java
c++ -g -Wall -O3 -shared -fPIC -std=c++0x -I${JAVA_HOME}/include/ -I${JAVA_HOME}/include/linux -I${RAMCLOUD_HOME}/src/ -I${RAMCLOUD_HOME}/obj.${RAMCLOUD_BRANCH}/ -I${RAMCLOUD_HOME}/logcabin/ -I${RAMCLOUD_HOME}/gtest/include/ -L${RAMCLOUD_HOME}/obj.${RAMCLOUD_BRANCH} -o libedu_stanford_ramcloud_JRamCloud.so edu_stanford_ramcloud_JRamCloud.cc -lramcloud
ln -s `pwd`/libedu_stanford_ramcloud_JRamCloud.so ${RAMCLOUD_HOME}/obj.${RAMCLOUD_BRANCH}
