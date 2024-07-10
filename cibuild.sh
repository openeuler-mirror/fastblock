./install-deps.sh
if [ $? != 0 ];then
    exit 1
fi

./build.sh -t Release -c monitor
if [ $? != 0 ];then
    exit 1
fi

./build.sh -t Release -c osd
if [ $? != 0 ];then
    exit 1
fi

exit 0