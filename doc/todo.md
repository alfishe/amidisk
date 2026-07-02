1) add new disk image creation (single partition whole disk by default + any multi-partition valid combination), any supported FS or reserving partitions, optional format with selected FS. design first
2) We need command to print any paritition stats - size, free, used, fs type, label, any other stats per partition, including total folders / file counts etc. Maybe something like:
amidisk info image.adf, it will print all info as it does now, but for partitions it will print: 
    Part:     3 2.51GB   2GB free  320MB used  PFS3  "workbench"
    Part:     4 1.91GB   0B free  1.91GB used  SFS   "games"
3) if we specify partition locator then we'll print more detailed information about each partition