package user

type HashKey struct {
	Key []byte
}
type BlockKey struct {
	Key []byte
}

type ProtectiveKey struct {
	Hashkey  HashKey
	Blockkey BlockKey
}

type User struct {
	Name         string
	Password     string
	Email        string
	Securecookie ProtectiveKey
}
