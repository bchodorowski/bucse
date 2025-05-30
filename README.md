# bucse -- Back Up with Client Side Encryption
`bucse` is a (network) filesystem that uses client side encryption
(a.k.a. zero-knowledge encryption). It allows making backups easily on
a server or in a cloud without giving data access to anyone else
(including server/cloud administrator).

# Demonstration
![Demonstration GIF](bucse.gif)

# Similar software
- [EncFS](https://github.com/vgough/encfs)
- [Rclone](https://github.com/rclone/rclone)
- [restic](https://github.com/restic/restic)

# Disclaimers
- Experimental Software: This software is currently in an experimental phase.
  Use it with caution, as there is a risk of data loss due to potential
  implementation errors.
- Encryption Warning: If you encrypt your repository and forget the password,
  your data will be irretrievably lost. This behavior is by design and not a
  software defect.
- Security Notice: While reasonable efforts have been made to ensure the
  security of this software, no software can be guaranteed to be completely
  secure. Use at your own risk.

