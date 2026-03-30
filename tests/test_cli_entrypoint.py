from __future__ import annotations
import subprocess
import unittest
from pathlib import Path
class CliEntrypointTests(unittest.TestCase):
    def test_fleaux_launcher_runs_program(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        completed = subprocess.run(
            [str(launcher), "test.fleaux"],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Executed generated module:", completed.stdout)
if __name__ == "__main__":
    unittest.main()
