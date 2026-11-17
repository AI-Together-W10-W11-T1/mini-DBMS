"""Check freshness signals and command result preservation, without network/tools."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import dev_tools as tools


class ToolChecks(unittest.TestCase):
    def test_new_wiki_page_requires_review_registration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wiki = root / 'wiki'
            wiki.mkdir()
            (wiki / 'new.md').write_text('not reviewed', encoding='utf-8')
            with patch.object(tools, 'ROOT', root), patch.object(tools, 'WIKI', wiki):
                row = tools.wiki_status()['wiki/new.md']
                self.assertTrue(row['page_changed'])
                self.assertIsNone(row['reviewed_at'])

    def test_changed_detects_added_modified_deleted(self):
        self.assertEqual(tools.changed({'old': 'x', 'same': 'y', 'edit': 'a'},
                                       {'new': 'z', 'same': 'y', 'edit': 'b'}),
                         ['edit', 'new', 'old'])

    def test_wiki_flags_source_changes_and_unreviewed_page(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wiki = root / 'wiki'
            wiki.mkdir()
            source = root / 'source.ts'
            source.write_text('old', encoding='utf-8')
            page = wiki / 'page.md'
            page.write_text('reviewed', encoding='utf-8')
            tools.write_json(wiki / 'sources.json', {'wiki/page.md': {
                'reviewed_at': 'test', 'page_sha256': tools.digest(page),
                'sources': {'source.ts': tools.digest(source)}}})
            with patch.object(tools, 'ROOT', root), patch.object(tools, 'WIKI', wiki):
                self.assertFalse(tools.wiki_status()['wiki/page.md']['changed_sources'])
                source.write_text('changed', encoding='utf-8')
                page.write_text('unreviewed edit', encoding='utf-8')
                row = tools.wiki_status()['wiki/page.md']
                self.assertEqual(row['changed_sources'], ['source.ts'])
                self.assertTrue(row['page_changed'])
                source.unlink()
                page.unlink()
                self.assertTrue(tools.wiki_status()['wiki/page.md']['missing_page'])
                self.assertEqual(tools.wiki_status()['wiki/page.md']['changed_sources'], ['source.ts'])

    def test_wrapper_preserves_nonzero_exit_and_both_streams(self):
        with tempfile.TemporaryDirectory() as directory:
            stdout = io.TextIOWrapper(io.BytesIO(), encoding='utf-8')
            stderr = io.TextIOWrapper(io.BytesIO(), encoding='utf-8')
            with patch.object(tools, 'CACHE', Path(directory)), \
                    patch.object(tools.sys, 'stdout', stdout), \
                    patch.object(tools.sys, 'stderr', stderr):
                code = tools.execute('raw', [sys.executable, '-c',
                    'import sys; print("out"); print("err", file=sys.stderr); sys.exit(7)'])
            self.assertEqual(code, 7)
            self.assertIn(b'out', stdout.buffer.getvalue())
            self.assertIn(b'err', stderr.buffer.getvalue())
            event = json.loads((Path(directory) / 'events.jsonl').read_text(encoding='utf-8'))
            self.assertEqual(event['exit_code'], 7)
            self.assertGreater(event['output_bytes'], 0)


if __name__ == '__main__':
    unittest.main()
