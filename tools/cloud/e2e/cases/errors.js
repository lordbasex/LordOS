// errors.js — add cases that must fail with a clear code and leave nothing behind
const key = (n) => String(lord.fs.read(`/etc/lordos-e2e/keys/${n}`)).trim().split('\n').join('\\n');
await cloud.providers();
const out = {};
const run = async (k, n, f) => {
  const t = Date.now(); let r = await cloud.add(k, n, f); const trail = [];
  while (r && r.question) { trail.push(r.question.kind || r.question.name); r = await cloud.answer(r.question.state, 'trust'); }
  await sleep(3000);
  const left = (S().accounts || []).filter((a) => a.name === n).length;
  let folder = false; try { lord.fs.stat(C(n)); folder = true; } catch (e) { /* gone */ }
  out[n] = { r: JSON.stringify(r).slice(0, 220), trail, ms: Date.now() - t, leftInState: left, folderLeft: folder };
};
await run('webdav', 'E Dav WrongPw', { url: 'http://127.0.0.1:8080/dav/', user: 'davuser', pass: 'nope' });
await run('webdav', 'E Dav WrongPort', { url: 'http://127.0.0.1:8099/dav/', user: 'davuser', pass: 'E2e-dav-0001' });
await run('webdav', 'E Dav Unreach', { url: 'http://192.0.2.1/dav/', user: 'davuser', pass: 'x' });
await run('webdav', 'E Dav BadUrl', { url: 'httpÑ--127.0.0.1Ñ8080-dav-', user: 'davuser', pass: 'x' });
await run('sftp', 'E Sftp WrongPw', { host: '127.0.0.1', port: '2222', user: 'e2epw', pass: 'nope' });
await run('sftp', 'E Sftp WrongKey', { host: '127.0.0.1', port: '2222', user: 'e2ekey', key_pem: key('id_other') });
await run('sftp', 'E Sftp KeyPassWrong', { host: '127.0.0.1', port: '2222', user: 'e2ekey', key_pem: key('id_pass'), key_file_pass: 'nope' });
await run('sftp', 'E Sftp WrongPort', { host: '127.0.0.1', port: '2299', user: 'e2epw', pass: 'E2e-pw-0001' });
await run('sftp', 'E Sftp Unreach', { host: '192.0.2.1', port: '22', user: 'e2epw', pass: 'E2e-pw-0001' });
await run('sftp', 'E Sftp SshCmd', { host: '127.0.0.1', port: '2222', user: 'e2epw', pass: 'x', ssh: 'sh -c "id > /tmp/e2e-ssh-option-ran3"' });
await run('sftp', 'E Sftp KeyFile', { host: '127.0.0.1', port: '2222', user: 'e2ekey', key_file: '/etc/lordos-e2e/keys/id_plain' });
await run('sftp', 'E Sftp PubkeyFile', { host: '127.0.0.1', port: '2222', user: 'e2ekey', pubkey_file: '/etc/passwd' });
await run('webdav', 'E Dav TokenCmd', { url: 'http://127.0.0.1:8080/dav/', bearer_token_command: 'sh -c "id > /tmp/e2e-token-cmd-ran3"' });
await run('s3', 'E MinIO WrongSecret', { provider: 'Minio', access_key_id: 'e2eappkey', secret_access_key: 'nope', endpoint: 'http://127.0.0.1:9000' });
await run('s3', 'E MinIO WrongPort', { provider: 'Minio', access_key_id: 'e2eappkey', secret_access_key: 'x', endpoint: 'http://127.0.0.1:9099' });
await run('ftp', 'E FTP WrongPw', { host: '127.0.0.1', port: '2121', user: 'e2epw', pass: 'nope' });
await run('smb', 'E SMB WrongPw', { host: '127.0.0.1', user: 'e2epw', pass: 'nope' });
return out;
