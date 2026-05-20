"""
IOWarp FUSE adapter — bare-metal only.

Mounts the CTE-backed virtual filesystem at a configured path by launching
the `clio_cte_fuse` binary (built with CLIO_CTE_ENABLE_FUSE_ADAPTER=ON).
"""
from jarvis_cd.core.pkg import Service
from jarvis_cd.shell import Exec, PsshExecInfo
from jarvis_cd.shell.process import Mkdir, Kill
import time


class ClioCteLibfuse(Service):
    """IOWarp FUSE adapter — mounts the CTE filesystem at a configured path."""

    def _init(self):
        self.binary = 'clio_cte_fuse'

    def _configure_menu(self):
        return [
            {
                'name': 'mountpoint',
                'msg': 'Absolute path to mount the CTE filesystem.',
                'type': str,
                'default': '${HOME}/clio_cte',
            },
            {
                'name': 'log_level',
                'msg': 'HSHM log level for the FUSE daemon',
                'type': str,
                'choices': ['debug', 'info', 'warning', 'error'],
                'default': 'info',
            },
            {
                'name': 'extra_fuse_args',
                'msg': 'Extra CLI flags forwarded to clio_cte_fuse / libfuse.',
                'type': str,
                'default': '-f',
            },
            {
                'name': 'sleep',
                'msg': 'Seconds to wait after launch for the FUSE handshake.',
                'type': int,
                'default': 2,
            },
        ]

    def _configure(self, **kwargs):
        super()._configure(**kwargs)
        self.setenv('CTP_LOG_LEVEL', self.config['log_level'])
        self.setenv('CHI_WITH_RUNTIME', '0')

    def start(self):
        mp = self.config['mountpoint']
        extra = self.config.get('extra_fuse_args', '').strip()

        # Hack: idempotent tear-down before bring-up so a prior
        # scancel-killed run that left a dangling FUSE mount doesn't
        # poison this run's Mkdir/clio_cte_fuse with "Transport endpoint
        # is not connected". stop() is already a fusermount3 -u + Kill
        # of the binary; calling it here just makes start() idempotent.
        self.stop()

        Mkdir(mp, PsshExecInfo(env=self.mod_env, hostfile=self.hostfile)).run()

        fuse_cmd = f'{self.binary} {mp} {extra}'.strip()
        self.log(f"Mounting IOWarp CTE FUSE at {mp}: {fuse_cmd}")
        Exec(fuse_cmd, PsshExecInfo(
            env=self.mod_env, hostfile=self.hostfile,
            exec_async=True)).run()
        time.sleep(self.config.get('sleep', 2))

    def stop(self):
        mp = self.config['mountpoint']
        self.log(f"Unmounting {mp}")
        Exec(f'fusermount3 -u {mp}', PsshExecInfo(
            env=self.mod_env, hostfile=self.hostfile)).run()
        Kill(self.binary, PsshExecInfo(
            env=self.mod_env, hostfile=self.hostfile)).run()

    def clean(self):
        self.stop()
