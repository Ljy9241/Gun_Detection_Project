"""Runtime configuration; defaults only bind a local, isolated database."""
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    database_url: str
    max_body_bytes: int = 65536

    @classmethod
    def from_env(cls):
        default_path = Path(__file__).resolve().parent / 'runtime' / 'receiver.db'
        return cls(os.environ.get('GUN_DATABASE_URL', f'sqlite:///{default_path.as_posix()}'))
