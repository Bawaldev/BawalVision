package user

func CreateUser(name, email, password string) *User {
	pk := NewProtectiveKey()

	return &User{
		Name:         name,
		Email:        email,
		Password:     password,
		Securecookie: *pk,
	}
}
func mapToUser(m map[string]string) User {
	return User{
		Name:     m["name"],
		Password: m["password"],
		Email:    m["email"],
	}
}
