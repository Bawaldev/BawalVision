package user_handler

func CreateUser(name, email, password string) *User {
	hashKey, blockKey := GenerateKeys()

	return &User{
		Name:     name,
		Email:    email,
		Password: password,
		HashKey:  hashKey,
		BlockKey: blockKey,
	}
}
func mapToUser(m map[string]string) User {
	return User{
		Name:     m["name"],
		Password: m["password"],
		Email:    m["email"],
	}
}
