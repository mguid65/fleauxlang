from __future__ import annotations
import shutil
import subprocess
import tempfile
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

    def test_fleaux_launcher_can_emit_graph(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            graph_out = Path(tmp_dir) / "program.dot"
            completed = subprocess.run(
                [str(launcher), "test.fleaux", "--emit-graph", "--graph-out", str(graph_out)],
                cwd=repo_root,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(graph_out.exists())
            dot = graph_out.read_text(encoding="utf-8")
            self.assertIn("digraph fleaux", dot)
            self.assertIn("Emitted graph:", completed.stdout)

    def test_fleaux_launcher_graph_format_svg(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            graph_out = Path(tmp_dir) / "program.svg"
            completed = subprocess.run(
                [
                    str(launcher),
                    "test.fleaux",
                    "--emit-graph",
                    "--graph-format",
                    "svg",
                    "--graph-out",
                    str(graph_out),
                ],
                cwd=repo_root,
                text=True,
                capture_output=True,
                check=False,
            )

            if shutil.which("dot") is None:
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("Graph emission failed", completed.stdout)
            else:
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertTrue(graph_out.exists())
                svg = graph_out.read_text(encoding="utf-8")
                self.assertIn("<svg", svg)

    def test_fleaux_launcher_graph_only_skips_execution(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            graph_out = Path(tmp_dir) / "program.dot"
            completed = subprocess.run(
                [
                    str(launcher),
                    "test.fleaux",
                    "--emit-graph",
                    "--graph-only",
                    "--graph-out",
                    str(graph_out),
                ],
                cwd=repo_root,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(graph_out.exists())
            self.assertIn("Emitted graph:", completed.stdout)
            self.assertIn("Skipped execution", completed.stdout)

    def test_fleaux_launcher_graph_only_requires_emit_graph(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        completed = subprocess.run(
            [str(launcher), "test.fleaux", "--graph-only"],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("Graph-only mode requires --emit-graph.", completed.stdout)

    def test_fleaux_launcher_runs_external_program_with_bundled_std(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "outside_std_test.fleaux"
            source.write_text(
                "import Std;\n"
                "(1, 2) -> Std.Add -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source)],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("3", completed.stdout)

    def test_fleaux_launcher_forwards_runtime_args_to_cpp_backend(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "forward_args_cpp_backend.fleaux"
            source.write_text(
                "import Std;\n"
                "() -> Std.GetArgs -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--", "one", "two"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("one", completed.stdout)
            self.assertIn("two", completed.stdout)

    def test_fleaux_launcher_no_run_succeeds(self) -> None:
        if shutil.which("c++") is None and shutil.which("g++") is None and shutil.which("clang++") is None:
            self.skipTest("c++ compiler is required")
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        completed = subprocess.run(
            [str(launcher), "test.fleaux", "--no-run"],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
