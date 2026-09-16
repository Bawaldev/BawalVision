package user_handler

import (
	"net/http"

	"github.com/google/uuid"

	"github.com/gofiber/fiber/v2"
	securecookie "github.com/gorilla/securecookie"
)

var s = securecookie.New(
	securecookie.GenerateRandomKey(64),
	securecookie.GenerateRandomKey(32),
)

func NewProtectiveKey() *ProtectiveKey {
	return &ProtectiveKey{
		Hashkey:  HashKey{Key: securecookie.GenerateRandomKey(64)},  // HMAC-SHA512
		Blockkey: BlockKey{Key: securecookie.GenerateRandomKey(32)}, // AES-256
	}
}

func setSecureCookieHandler(w http.ResponseWriter, r *http.Request, c *fiber.Ctx) {
	userID := uuid.New().String()

	value := mapToUser(map[string]string{
		"user_id": userID,
		"role":    "user",
	})

	if encoded, err := s.Encode("session", value); err == nil {
		cookie := &http.Cookie{
			Name:     "session",
			Value:    encoded,
			Path:     "/",
			HttpOnly: true,
			Secure:   true,
			SameSite: http.SameSiteLaxMode,
		}
		http.SetCookie(w, cookie)
		w.Write([]byte("Secure signed/encrypted cookie set!"))
	}
}
