Put an `authorized_keys` file here to build a **developer** image:

    make bootimg MMK_DEV=1

That stages the key at `/root/.ssh/authorized_keys` inside the ramdisk so you can
SSH into the panel over Dropbear. No key is committed to this repository, and a
normal (non-`MMK_DEV`) build ships no `authorized_keys` at all.
