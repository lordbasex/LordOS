// adds.js — every good account of the bench, as the UI would add them (trust questions answered 'trust').
// The keys are read from /etc/lordos-e2e/keys (0644, throwaway) and sent as key_pem on one line, as the form does.
const key = (n) => String(lord.fs.read(`/etc/lordos-e2e/keys/${n}`)).trim().split('\n').join('\\n');
await cloud.providers();
const out = {};
const add = async (k, n, f) => {
  const t = Date.now(); let r = await cloud.add(k, n, f); const trail = [];
  while (r && r.question) { trail.push(r.question.kind || r.question.name); r = await cloud.answer(r.question.state, 'trust'); }
  const a = r && r.id ? await mounted(n, 30000) : null;
  out[n] = { r, trail, ms: Date.now() - t, state: a ? [a.state, a.mounted, a.error] : null };
};
const PFX = '__PFX__';
await add('sftp', `${PFX}SftpPw`, { host: '127.0.0.1', port: '2222', user: 'e2epw', pass: 'E2e-pw-0001' });
await add('sftp', `${PFX}SftpKey`, { host: '127.0.0.1', port: '2222', user: 'e2ekey', key_pem: key('id_plain') });
await add('sftp', `${PFX}SftpKeyPass`, { host: '127.0.0.1', port: '2222', user: 'e2ekey', key_pem: key('id_pass'), key_file_pass: 'E2e-key-pass-0001' });
await add('webdav', `${PFX}DavHttp`, { url: 'http://127.0.0.1:8080/dav/', user: 'davuser', pass: 'E2e-dav-0001' });
await add('webdav', `${PFX}DavHttpsCA`, { url: 'https://127.0.0.1:8444/dav/', user: 'davuser', pass: 'E2e-dav-0001' });
await add('webdav', `${PFX}DavSelfSigned`, { url: 'https://127.0.0.1:8443/dav/', user: 'davuser', pass: 'E2e-dav-0001' });
await add('s3', `${PFX}MinIO`, { provider: 'Minio', access_key_id: 'e2eappkey', secret_access_key: 'E2e-minio-app-secret-0001', endpoint: 'http://127.0.0.1:9000' });
await add('s3', `${PFX}MinIO TLS`, { provider: 'Minio', access_key_id: 'e2eappkey', secret_access_key: 'E2e-minio-app-secret-0001', endpoint: 'https://127.0.0.1:9443' });
await add('s3', `${PFX}RcloneS3`, { provider: 'Rclone', access_key_id: 'e2ers3key', secret_access_key: 'E2e-rs3-secret-0001', endpoint: 'http://127.0.0.1:9100' });
await add('ftp', `${PFX}FTP`, { host: '127.0.0.1', port: '2121', user: 'e2epw', pass: 'E2e-pw-0001' });
await add('smb', `${PFX}SMB`, { host: '127.0.0.1', user: 'e2epw', pass: 'E2e-pw-0001' });
return out;
