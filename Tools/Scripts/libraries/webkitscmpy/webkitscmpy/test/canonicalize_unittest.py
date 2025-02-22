# Copyright (C) 2020-2022 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1.  Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
# 2.  Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR
# ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import os
import unittest
from io import StringIO

from webkitcorepy import OutputCapture, testing
from webkitcorepy.mocks import Time as MockTime
from webkitscmpy import program, mocks, local, Commit, Contributor
from webkitscmpy.program.canonicalize.message import rewrite_message


class TestCanonicalizeProgam(testing.PathTestCase):
    basepath = 'mock/repository'

    def setUp(self):
        super().setUp()
        os.mkdir(os.path.join(self.path, '.git'))
        os.mkdir(os.path.join(self.path, '.svn'))

    def test_invalid_svn(self):
        with OutputCapture() as captured, mocks.local.Git(), mocks.local.Svn(self.path), MockTime:
            self.assertEqual(1, program.main(
                args=('canonicalize',),
                path=self.path,
            ))

        self.assertEqual(captured.stderr.getvalue(), 'Commits can only be canonicalized on a Git repository\n')

    def test_invalid_None(self):
        with OutputCapture() as captured, mocks.local.Git(), mocks.local.Svn(), MockTime:
            self.assertEqual(1, program.main(
                args=('canonicalize',),
                path=self.path,
            ))

        self.assertEqual(captured.stderr.getvalue(), 'No repository provided\n')

    def test_no_commits(self):
        with OutputCapture() as captured, mocks.local.Git(self.path), mocks.local.Svn(), MockTime:
            self.assertEqual(0, program.main(
                args=('canonicalize',),
                path=self.path,
            ))

        self.assertEqual(captured.stdout.getvalue(), 'No local commits to be edited\n')

    def test_formated_identifier(self):
        with OutputCapture() as captured, mocks.local.Git(self.path) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('\u017dan Dober\u0161ek', 'zdobersek@igalia.com')

            mock.commits[mock.default_branch].append(Commit(
                hash='38ea50d28ae394c9c8b80e13c3fb21f1c262871f',
                branch=mock.default_branch,
                author=Contributor('\u017dan Dober\u0161ek', emails=['zdobersek@igalia.com']),
                identifier=mock.commits[mock.default_branch][-1].identifier + 1,
                timestamp=1601669000,
                message='New commit\n',
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', '-v',),
                path=self.path,
                contributors=contirbutors,
                identifier_template='Canonical link: https://commits.webkit.org/{}',
            ))

            commit = local.Git(self.path).commit(branch=mock.default_branch)
            self.assertEqual(commit.author, contirbutors['zdobersek@igalia.com'])
            self.assertEqual(commit.message, 'New commit\n\nCanonical link: https://commits.webkit.org/6@main')

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 38ea50d28ae394c9c8b80e13c3fb21f1c262871f (1/1) (--- seconds passed, remaining --- predicted)\n'
            'Overwriting 38ea50d28ae394c9c8b80e13c3fb21f1c262871f\n'
            '1 commit successfully canonicalized!\n',
        )

    def test_existing_identifier(self):
        with OutputCapture() as captured, mocks.local.Git(self.path) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            mock.commits[mock.default_branch].append(Commit(
                hash='38ea50d28ae394c9c8b80e13c3fb21f1c262871f',
                branch=mock.default_branch,
                author=Contributor('Jonathan Bedard', emails=['jbedard@apple.com']),
                identifier=mock.commits[mock.default_branch][-1].identifier + 1,
                timestamp=1601668000,
                message='New commit\n\nIdentifier: {}@{}'.format(
                    mock.commits[mock.default_branch][-1].identifier + 1,
                    mock.default_branch,
                ),
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', '-v',),
                path=self.path,
                contributors=contirbutors,
            ))

            commit = local.Git(self.path).commit(branch=mock.default_branch)
            self.assertEqual(commit.author, contirbutors['jbedard@apple.com'])
            self.assertEqual(commit.message, 'New commit\n\nIdentifier: 6@main')

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 38ea50d28ae394c9c8b80e13c3fb21f1c262871f (1/1) (--- seconds passed, remaining --- predicted)\n'
            'Overwriting 38ea50d28ae394c9c8b80e13c3fb21f1c262871f\n'
            '1 commit successfully canonicalized!\n',
        )

    def test_git_svn(self):
        with OutputCapture() as captured, mocks.local.Git(self.path, git_svn=True) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            mock.commits[mock.default_branch].append(Commit(
                hash='766609276fe201e7ce2c69994e113d979d2148ac',
                branch=mock.default_branch,
                author=Contributor('jbedard@apple.com', emails=['jbedard@apple.com']),
                identifier=mock.commits[mock.default_branch][-1].identifier + 1,
                timestamp=1601668000,
                revision=9,
                message='New commit\n',
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', '-vv'),
                path=self.path,
                contributors=contirbutors,
            ))

            commit = local.Git(self.path).commit(branch=mock.default_branch)
            self.assertEqual(commit.author, contirbutors['jbedard@apple.com'])
            self.assertEqual(
                commit.message,
                'New commit\n\n'
                'Identifier: 6@main\n'
                'git-svn-id: https://svn.example.org/repository/repository/trunk@9 268f45cc-cd09-0410-ab3c-d52691b4dbfc',
            )

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 766609276fe201e7ce2c69994e113d979d2148ac (1/1) (--- seconds passed, remaining --- predicted)\n'
            'Overwriting 766609276fe201e7ce2c69994e113d979d2148ac\n'
            '    GIT_AUTHOR_NAME=Jonathan Bedard\n'
            '    GIT_AUTHOR_EMAIL=jbedard@apple.com\n'
            '    GIT_COMMITTER_NAME=Jonathan Bedard\n'
            '    GIT_COMMITTER_EMAIL=jbedard@apple.com\n'
            '1 commit successfully canonicalized!\n',
        )

    def test_git_svn_existing(self):
        with OutputCapture() as captured, mocks.local.Git(self.path, git_svn=True) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            mock.commits[mock.default_branch].append(Commit(
                hash='766609276fe201e7ce2c69994e113d979d2148ac',
                branch=mock.default_branch,
                author=Contributor('jbedard@apple.com', emails=['jbedard@apple.com']),
                identifier=mock.commits[mock.default_branch][-1].identifier + 1,
                timestamp=1601668000,
                revision=9,
                message='New commit\nIdentifier: 6@main\n\n',
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', '-vv'),
                path=self.path,
                contributors=contirbutors,
            ))

            commit = local.Git(self.path).commit(branch=mock.default_branch)
            self.assertEqual(commit.author, contirbutors['jbedard@apple.com'])
            self.assertEqual(
                commit.message,
                'New commit\n\n'
                'Identifier: 6@main\n'
                'git-svn-id: https://svn.example.org/repository/repository/trunk@9 268f45cc-cd09-0410-ab3c-d52691b4dbfc',
            )

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 766609276fe201e7ce2c69994e113d979d2148ac (1/1) (--- seconds passed, remaining --- predicted)\n'
            'Overwriting 766609276fe201e7ce2c69994e113d979d2148ac\n'
            '    GIT_AUTHOR_NAME=Jonathan Bedard\n'
            '    GIT_AUTHOR_EMAIL=jbedard@apple.com\n'
            '    GIT_COMMITTER_NAME=Jonathan Bedard\n'
            '    GIT_COMMITTER_EMAIL=jbedard@apple.com\n'
            '1 commit successfully canonicalized!\n',
        )

    def test_git_svn_existing_merge_queue(self):
        with OutputCapture() as captured, mocks.local.Git(self.path, git_svn=True) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            mock.commits[mock.default_branch].append(Commit(
                hash='766609276fe201e7ce2c69994e113d979d2148ac',
                branch=mock.default_branch,
                author=Contributor('jbedard@apple.com', emails=['jbedard@apple.com']),
                identifier=mock.commits[mock.default_branch][-1].identifier + 1,
                timestamp=1601668000,
                revision=9,
                message='New commit\nIdentifier: 6@main\n\n\ngit-svn-id: https://svn.example.org/repository/repository/trunk@9 268f45cc-cd09-0410-ab3c-d52691b4dbfc',
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', '-vv'),
                path=self.path,
                contributors=contirbutors,
            ))

            commit = local.Git(self.path).commit(branch=mock.default_branch)
            self.assertEqual(commit.author, contirbutors['jbedard@apple.com'])

            self.assertEqual(
                commit.message,
                'New commit\n\n'
                'Identifier: 6@main\n'
                'git-svn-id: https://svn.example.org/repository/repository/trunk@9 268f45cc-cd09-0410-ab3c-d52691b4dbfc',
            )

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 766609276fe201e7ce2c69994e113d979d2148ac (1/1) (--- seconds passed, remaining --- predicted)\n'
            'Overwriting 766609276fe201e7ce2c69994e113d979d2148ac\n'
            '    GIT_AUTHOR_NAME=Jonathan Bedard\n'
            '    GIT_AUTHOR_EMAIL=jbedard@apple.com\n'
            '    GIT_COMMITTER_NAME=Jonathan Bedard\n'
            '    GIT_COMMITTER_EMAIL=jbedard@apple.com\n'
            '1 commit successfully canonicalized!\n',
        )

    def test_branch_commits(self):
        with OutputCapture() as captured, mocks.local.Git(self.path) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            local.Git(self.path).checkout('branch-a')
            mock.commits['branch-a'].append(Commit(
                hash='f93138e3bf1d5ecca25fc0844b7a2a78b8e00aae',
                branch='branch-a',
                author=Contributor('jbedard@apple.com', emails=['jbedard@apple.com']),
                branch_point=mock.commits['branch-a'][-1].branch_point,
                identifier=mock.commits['branch-a'][-1].identifier + 1,
                timestamp=1601668000,
                message='New commit 1\n',
            ))
            mock.commits['branch-a'].append(Commit(
                hash='0148c0df0faf248aa133d6d5ad911d7cb1b56a5b',
                branch='branch-a',
                author=Contributor('jbedard@apple.com', emails=['jbedard@apple.com']),
                branch_point=mock.commits['branch-a'][-1].branch_point,
                identifier=mock.commits['branch-a'][-1].identifier + 1,
                timestamp=1601669000,
                message='New commit 2\n',
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', ),
                path=self.path,
                contributors=contirbutors,
            ))

            commit_a = local.Git(self.path).commit(branch='branch-a~1')
            self.assertEqual(commit_a.author, contirbutors['jbedard@apple.com'])
            self.assertEqual(commit_a.message, 'New commit 1\n\nIdentifier: 2.3@branch-a')

            commit_b = local.Git(self.path).commit(branch='branch-a')
            self.assertEqual(commit_b.author, contirbutors['jbedard@apple.com'])
            self.assertEqual(commit_b.message, 'New commit 2\n\nIdentifier: 2.4@branch-a')

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite f93138e3bf1d5ecca25fc0844b7a2a78b8e00aae (1/2) (--- seconds passed, remaining --- predicted)\n'
            'Rewrite 0148c0df0faf248aa133d6d5ad911d7cb1b56a5b (2/2) (--- seconds passed, remaining --- predicted)\n'
            '2 commits successfully canonicalized!\n',
        )

    def test_number(self):
        with OutputCapture() as captured, mocks.local.Git(self.path), mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            self.assertEqual(0, program.main(
                args=('canonicalize', '--number', '3'),
                path=self.path,
                contributors=contirbutors,
            ))

            self.assertEqual(local.Git(self.path).commit(identifier='5@main').message, 'Patch Series\n\nIdentifier: 5@main')
            self.assertEqual(local.Git(self.path).commit(identifier='4@main').message, '8th commit\n\nIdentifier: 4@main')
            self.assertEqual(local.Git(self.path).commit(identifier='3@main').message, '4th commit\n\nIdentifier: 3@main')

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 1abe25b443e985f93b90d830e4a7e3731336af4d (1/3) (--- seconds passed, remaining --- predicted)\n'
            'Rewrite bae5d1e90999d4f916a8a15810ccfa43f37a2fd6 (2/3) (--- seconds passed, remaining --- predicted)\n'
            'Rewrite d8bce26fa65c6fc8f39c17927abb77f69fab82fc (3/3) (--- seconds passed, remaining --- predicted)\n'
            '3 commits successfully canonicalized!\n',
        )

    def test_alternate_trailer(self):
        with OutputCapture() as captured, mocks.local.Git(self.path) as mock, mocks.local.Svn(), MockTime:
            contirbutors = Contributor.Mapping()
            contirbutors.create('Jonathan Bedard', 'jbedard@apple.com')

            mock.commits[mock.default_branch].append(Commit(
                hash='766609276fe201e7ce2c69994e113d979d2148ac',
                branch=mock.default_branch,
                author=Contributor('jbedard@apple.com', emails=['jbedard@apple.com']),
                identifier=mock.commits[mock.default_branch][-1].identifier + 1,
                timestamp=1601668000,
                message='New commit\n\ntrailer: some metadata\n',
            ))

            self.assertEqual(0, program.main(
                args=('canonicalize', '-vv'),
                path=self.path,
                contributors=contirbutors,
            ))

            commit = local.Git(self.path).commit(branch=mock.default_branch)
            self.assertEqual(commit.author, contirbutors['jbedard@apple.com'])
            self.assertEqual(
                commit.message,
                'New commit\n\n'
                'trailer: some metadata\n'
                'Identifier: 6@main',
            )

        self.assertEqual(
            captured.stdout.getvalue(),
            'Rewrite 766609276fe201e7ce2c69994e113d979d2148ac (1/1) (--- seconds passed, remaining --- predicted)\n'
            'Overwriting 766609276fe201e7ce2c69994e113d979d2148ac\n'
            '    GIT_AUTHOR_NAME=Jonathan Bedard\n'
            '    GIT_AUTHOR_EMAIL=jbedard@apple.com\n'
            '    GIT_COMMITTER_NAME=Jonathan Bedard\n'
            '    GIT_COMMITTER_EMAIL=jbedard@apple.com\n'
            '1 commit successfully canonicalized!\n',
        )


class TestCanonicalizeMessage(unittest.TestCase):
    def assert_canonicalized_commit_message(self, *, message, expected):
        stdin = StringIO(message)
        stdout = StringIO()

        commit = Commit(
            hash='38ea50d28ae394c9c8b80e13c3fb21f1c262871f',
            branch='main',
            author=Contributor('Jonathan Bedard', emails=['jbedard@apple.com']),
            identifier='6@main',
            timestamp=1601669000,
            message=message,
        )

        rewrite_message(stdin, stdout, commit, 'Identifier: {}')

        self.assertEqual(stdout.getvalue(), expected)

    def test_incomplete_line(self):
        # By POSIX, a line must end in new line character.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_multiple_existing_identifier_trailers(self):
        # Only the final trailer gets updated.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                'Identifier: 3@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                'Identifier: 6@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                'Identifier: 3@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                'Identifier: 6@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_multiple_existing_identifier_single_trailer(self):
        # Only the trailer gets updated.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Identifier: 3@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Identifier: 6@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Identifier: 3@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Identifier: 6@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_existing_non_trailer_identifier_regression(self):
        # This behaviour is wrong, see test_existing_non_trailer_identifier
        # below. Remove this test when the expectedFailure goes away.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Commit message body\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    @unittest.expectedFailure
    def test_existing_non_trailer_identifier(self):
        # A trailer gets added.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Commit message body\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_existing_non_trailer_identifier_long(self):
        # A trailer gets added.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Kinda\n'
                'long\n'
                'commit\n'
                'message\n'
                'body\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Kinda\n'
                'long\n'
                'commit\n'
                'message\n'
                'body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Kinda\n'
                'long\n'
                'commit\n'
                'message\n'
                'body\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Kinda\n'
                'long\n'
                'commit\n'
                'message\n'
                'body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_existing_identifier_and_non_trailer_identifier(self):
        # Only the trailer gets updated.
        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 3@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 3@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 3@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'Identifier: 6@main\n'
                '\n'
                'Commit message body\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_not_alternate_trailer(self):
        # Add a trailer group after the commit body.
        non_trailer_lines = [
            'not a trailer: line',
            '* a: b',
            '0a b: c',
            '[0]: https://example.com',
            '`a: b`',
            '`a`: b',
            'a b: https://example.com',
        ]

        for line in non_trailer_lines:
            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    '\n'
                    'Identifier: 6@main\n'
                ),
            )

    def test_partial_trailer_group_unknown_trailer_regression(self):
        # This behaviour is wrong, see test_partial_trailer_group_unknown_trailer
        # below. Remove this test when the expectedFailure goes away.
        non_trailer_lines = [
            'not a trailer: line',
            '* a: b',
            '0a b: c',
            '[0]: https://example.com',
            '`a: b`',
            '`a`: b',
            'a b: https://example.com',
        ]

        for line in non_trailer_lines:
            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    'trailer: some metadata\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    '\n'
                    'trailer: some metadata\n'
                    'Identifier: 6@main\n'
                ),
            )

            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    'trailer: some metadata\n'
                    f'{line}\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    'trailer: some metadata\n'
                    f'{line}\n'
                    '\n'
                    'Identifier: 6@main\n'
                ),
            )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'I proposed this change in:\n'
                'https://github.example.com/WebKit/WebKit/pull/19920\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'I proposed this change in:\n'
                '\n'
                'https://github.example.com/WebKit/WebKit/pull/19920\n'
                'Identifier: 6@main\n'
            ),
        )

    @unittest.expectedFailure
    def test_partial_trailer_group_unknown_trailer(self):
        # Add a trailer group after the commit body.
        non_trailer_lines = [
            'not a trailer: line',
            '* a: b',
            '0a b: c',
            '[0]: https://example.com',
            '`a: b`',
            '`a`: b',
            'a b: https://example.com',
        ]

        for line in non_trailer_lines:
            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    'trailer: some metadata\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    'trailer: some metadata\n'
                    '\n'
                    'Identifier: 6@main'
                ),
            )

            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    'trailer: some metadata\n'
                    f'{line}\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    'trailer: some metadata\n'
                    f'{line}\n'
                    '\n'
                    'Identifier: 6@main'
                ),
            )

        self.assert_canonicalized_commit_message(
            message=(
                'New commit\n'
                '\n'
                'I proposed this change in:\n'
                'https://github.example.com/WebKit/WebKit/pull/19920\n'
            ),
            expected=(
                'New commit\n'
                '\n'
                'I proposed this change in:\n'
                'https://github.example.com/WebKit/WebKit/pull/19920\n'
                '\n'
                'Identifier: 6@main\n'
            ),
        )

    def test_partial_trailer_group_known_trailer(self):
        # Move the existing trailers to a proper group and append.
        non_trailer_lines = [
            'not a trailer: line',
            '* a: b',
            '0a b: c',
            '[0]: https://example.com',
            '`a: b`',
            '`a`: b',
            'a b: https://example.com',
        ]

        for line in non_trailer_lines:
            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    'Signed-off-by: Heather Letty <heather.letty@example.com>\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    '\n'
                    'Signed-off-by: Heather Letty <heather.letty@example.com>\n'
                    'Identifier: 6@main\n'
                ),
            )

            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    'Signed-off-by: Heather Letty <heather.letty@example.com>\n'
                    f'{line}\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    'Signed-off-by: Heather Letty <heather.letty@example.com>\n'
                    f'{line}\n'
                    '\n'
                    'Identifier: 6@main\n'
                ),
            )

    def test_partial_trailer_group_known_identifier_trailer(self):
        # Move the existing trailers to a property group and append.
        non_trailer_lines = [
            'not a trailer: line',
            '* a: b',
            '0a b: c',
            '[0]: https://example.com',
            '`a: b`',
            '`a`: b',
            'a b: https://example.com',
        ]

        for line in non_trailer_lines:
            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    'Identifier: 3@main\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    '\n'
                    'Identifier: 6@main\n'
                ),
            )

            self.assert_canonicalized_commit_message(
                message=(
                    'New commit\n'
                    '\n'
                    'Identifier: 3@main\n'
                    f'{line}\n'
                ),
                expected=(
                    'New commit\n'
                    '\n'
                    f'{line}\n'
                    '\n'
                    'Identifier: 6@main\n'
                ),
            )
