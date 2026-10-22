# UB API 接口回归（无真实 UB 硬件）

`mock/` 是测试专用 API/ABI fixture，依据用户给定函数签名编写，不是厂商头文件。
其 flag 数值、类型布局只供本测试使用，不表示现场 SDK ABI。生产必须使用真实 SDK。
测试替身打印 UBSM_TEST_STUB，并用独占创建的本地文件模拟对象；拒绝有活动映射的释放。

从仓库根目录：

```sh
mkdir -p build/ub-api-fixture
c++ -std=c++17 -shared -fPIC -Itests/ub-api/mock \
  tests/ub-api/mock/sdk.cc -o build/ub-api-fixture/libubsm_sdk.so
c++ -std=c++17 -pthread -DSHM_LIB_WITH_UBSM \
  -Ishm-lib/include -Itests/ub-api/mock \
  tests/ub-api/contract.cc shm-lib/shm/ub.cc \
  -Lbuild/ub-api-fixture -Wl,-rpath,"$PWD/build/ub-api-fixture" \
  -lubsm_sdk -o build/ub-api-fixture/contract
UBSM_TEST_DIRECTORY="$PWD/build/ub-api-fixture/objects" build/ub-api-fixture/contract

UBSM_TEST_DIRECTORY="$PWD/build/ub-api-fixture/objects" \
python3 tests/two-process-ycsb/run.py --memory ub --ub-api ubsm \
  --ub-library "$PWD/build/ub-api-fixture/libubsm_sdk.so" \
  --ub-include "$PWD/tests/ub-api/mock" --provider-host test-owner \
  --shared-path ubsm-test --threads 1 2 --records 1000 --ops 2000 \
  --size-mb 1024 --mapping-bits 16 --output build/ubsm-regression

python3 tests/ub-api/failure_checks.py
```

输出目录必须不存在。contract 检查同名对象拒绝覆盖、错误返回地址清理和整段 unmap。
failure_checks 注入 map/unmap/finalize 失败，验证结果失败且原对象保留。
正常 runner 回归检查八种配置的两个进程、数据正确性、解除映射和对象释放。
这些测试不验证设备内存类型、厂商 ABI 或真实网络。

2026-10-20 回归结果：Intel 本机、arm-0 的 GCC 12.3.1 / glibc 2.39 Docker
均通过八种 UBSM fixture 配置（两个进程，线程数 1/2，各 2000 次操作，1000 条初始数据）。
所有成功 case 均完成 unmap/finalize 后才释放对象；另通过 Memlink 替身和普通文件模式回归。
错误注入覆盖 map、unmap、finalize；错误结果均保留对象。以上均不是真实双机 UB 测试。

`python3 tests/ub-api/latency_check.py` 编译三种后端的原子请求探针并检查目标文件：
UBSM/Memlink 不引用模拟用的 `clock_gettime`，非 UB 仍保留此依赖。
单机 UB 入口的两个 OPT case 另检查双方的 `UB_CAS_PROBE` 标记。
这验证代码路径，不证明真实 UB 页属性或硬件原子语义。

真实 SDK 的独立 init / NonCache / Cache 最小测试见 [SDK_SMOKE.md](SDK_SMOKE.md)。
