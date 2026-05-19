from jarvis_cd.core.pkg import Application
from jarvis_cd.shell import Exec, PsshExecInfo
import os


class WrpRedisBench(Application):
    """
    Redis throughput benchmark — apples-to-apples mirror of wrp_cte_bench.

    Drives the `wrp_redis_bench` executable (built when
    WRP_CORE_ENABLE_REDIS=ON) which shares its CLI/metrics with
    wrp_cte_bench via bench_common.h. Each client thread holds its own
    hiredis connection and issues SET (Put) / GET (Get) with --depth
    pipelining, mirroring CTE PutBlob/GetBlob. Supports --time-limit
    (run N seconds) and --max-total-blobs (global keyspace split
    evenly across threads, cycling).

    Connects to a Redis server via REDIS_HOST / REDIS_PORT.
    """

    def _init(self):
        self.benchmark_executable = 'wrp_redis_bench'
        self.output_file = None

    def _configure_menu(self):
        return [
            {
                'name': 'test_case',
                'msg': 'Benchmark test case to run',
                'type': str,
                'choices': ['Put', 'Get', 'PutGet'],
                'default': 'Put',
                'help': 'Put: SET, Get: GET, PutGet: SET+GET per key',
            },
            {
                'name': 'num_threads',
                'msg': 'Number of client threads (each its own connection)',
                'type': int,
                'default': 1,
                'help': 'Maps to wrp_redis_bench --threads',
            },
            {
                'name': 'depth',
                'msg': 'Pipelined requests in flight per thread',
                'type': int,
                'default': 1,
                'help': 'Maps to --depth (Redis command pipelining)',
            },
            {
                'name': 'io_size',
                'msg': 'Size of each value',
                'type': str,
                'default': '1m',
                'help': 'k/K (KB), m/M (MB), g/G (GB). e.g. 4k, 1m',
            },
            {
                'name': 'io_count',
                'msg': 'Ops per thread (ignored when time_limit > 0)',
                'type': int,
                'default': 1000,
                'help': 'Maps to --io-count',
            },
            {
                'name': 'time_limit',
                'msg': 'Run each phase for N seconds instead of io_count',
                'type': int,
                'default': 0,
                'help': '0 = use io_count; >0 = run that many seconds '
                        '(--time-limit)',
            },
            {
                'name': 'max_total_blobs',
                'msg': 'Cap TOTAL distinct keys across all threads',
                'type': int,
                'default': 0,
                'help': '0 = unbounded; else global keyspace split evenly '
                        'across threads (--max-total-blobs). Total unique '
                        'bytes = max_total_blobs * io_size, independent of '
                        'num_threads.',
            },
            {
                'name': 'redis_host',
                'msg': 'Redis server host',
                'type': str,
                'default': '127.0.0.1',
                'help': 'Exported as REDIS_HOST',
            },
            {
                'name': 'redis_port',
                'msg': 'Redis server port',
                'type': int,
                'default': 6379,
                'help': 'Exported as REDIS_PORT',
            },
            {
                'name': 'nprocs',
                'msg': 'Number of processes (Pssh)',
                'type': int,
                'default': 1,
            },
            {
                'name': 'ppn',
                'msg': 'Processes per node',
                'type': int,
                'default': 1,
            },
            {
                'name': 'output_file',
                'msg': 'Output file for benchmark results',
                'type': str,
                'default': '',
                'help': 'If empty, results go to stdout',
            },
        ]

    def _configure(self, **kwargs):
        self.log("Configuring Redis benchmark application...")

        if self.config['test_case'] not in ['Put', 'Get', 'PutGet']:
            raise ValueError(
                f"Invalid test_case: {self.config['test_case']}")
        if self.config['num_threads'] <= 0:
            raise ValueError("num_threads must be > 0")
        if self.config['depth'] <= 0:
            raise ValueError("depth must be > 0")
        if (self.config['time_limit'] <= 0 and
                self.config['io_count'] <= 0):
            raise ValueError("need io_count > 0 or time_limit > 0")

        if self.config['output_file']:
            self.output_file = os.path.join(self.shared_dir,
                                            self.config['output_file'])
            self.log(f"Results will be saved to: {self.output_file}")
        else:
            self.output_file = None

        # Redis connection for the benchmark process.
        self.setenv('REDIS_HOST', str(self.config['redis_host']))
        self.setenv('REDIS_PORT', str(self.config['redis_port']))

        self.log("Redis benchmark configuration completed")

    def start(self):
        self.log(f"Starting Redis benchmark: {self.config['test_case']}")

        # Semantic-flag CLI (bench_common.h). Use --time-limit when set,
        # otherwise --io-count.
        cmd = [
            self.benchmark_executable,
            '--op', str(self.config['test_case']),
            '--threads', str(self.config['num_threads']),
            '--depth', str(self.config['depth']),
            '--io-size', str(self.config['io_size']),
        ]
        if int(self.config['time_limit']) > 0:
            cmd += ['--time-limit', str(self.config['time_limit'])]
        else:
            cmd += ['--io-count', str(self.config['io_count'])]
        if int(self.config['max_total_blobs']) > 0:
            cmd += ['--max-total-blobs', str(self.config['max_total_blobs'])]

        exec_info = PsshExecInfo(
            env=self.mod_env,
            hostfile=self.hostfile,
            nprocs=self.config['nprocs'],
            ppn=self.config['ppn'],
        )

        cmd_str = ' '.join(cmd)
        if self.output_file:
            cmd_str += f' > {self.output_file} 2>&1'
        self.log(f"Executing: {cmd_str}")
        Exec(cmd_str, exec_info).run()
        self.log("Redis benchmark completed")

    def stop(self):
        self.log("WrpRedisBench is an application - runs to completion")
        return True

    def clean(self):
        if self.output_file and os.path.exists(self.output_file):
            try:
                os.remove(self.output_file)
                self.log(f"Removed: {self.output_file}")
            except Exception as e:
                self.log(f"Error removing output file: {e}")
