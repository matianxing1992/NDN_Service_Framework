if (process.argv[2] === 'echo') {
  process.stdin.pipe(process.stdout);
} else {
  setInterval(() => {}, 1000);
}
